/*
 * mod_gridfactory
 * 
 * Apache module providing a web interface to the gridfactory job database.
 * 
 * Copyright (c) 1008 Frederik Orellana, Niels Bohr Institute,
 * University of Copenhagen. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 * 
 * This program is based loosely on mod_authn_dbd of the Apache foundation,
 * http://svn.apache.org/viewvc/httpd/httpd/trunk/modules/aaa/mod_authn_dbd.c?revision=658046&view=markup.
 * mod_authn_dbd is covered by the pache License, Version 2.0,
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 *******************************************************************************
 * This module allows you to create a gridfactory web service for job pulling.
 * It requires mod_dbm to be loaded and enabled for the Location /grid/jobs.
 * 
 * 
 *  LoadModule gridfactory_module /usr/lib/apache2/modules/mod_gridfactory.so
 *  <Location /grid/jobs>
 *    SetHandler gridfactory
 *  </Location>
 * 
 */

#include "ap_provider.h"
#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_request.h"
#include "apr_lib.h"
#include "apr_dbd.h"
#include "mod_dbd.h"
#include "apr_strings.h"
#include "mod_auth.h"
#include "apr_md5.h"
#include "apu_version.h"

#include <mysql/mysql.h>

module AP_MODULE_DECLARE_DATA authn_dbd_module;

/* The sub-directory containing the jobs. */
static char* JOB_DIR = "/jobs/";

/* Keys in the has table of prepared statements. */
static char* LABEL1 = "gridfactory_dbd_1";
static char* LABEL2 = "gridfactory_dbd_2";

/* SQL query to get list of fields. */
static char* JOB_REC_SHOW_F_Q = "SHOW fields FROM `jobDefinition`";

/* SQL query to get all job records. */
static char* JOB_REC_SELECT_Q = "SELECT * FROM `jobDefinition`";

/* Prepared statement string to get job record. */
static char* JOB_REC_SELECT_PS = "SELECT * FROM `jobDefinition` WHERE identifier LIKE '%/?'";

/* Prepared statement string to update job record. */
static char* JOB_REC_UPDATE_PS = "UPDATE `jobDefinition` SET csStatus = '?', lastModified = '?' WHERE identifier LIKE '%/?'";

/* Optional function - look it up once in post_config. */
static ap_dbd_t *(*dbd_acquire_fn)(request_rec*) = NULL;
static void (*dbd_prepare_fn)(server_rec*, const char*, const char*) = NULL;

/* Max size of each field name. */
static int MAX_F_SIZE = 256;

/* Max size of all field names. */
static int MAX_T_F_SIZE = 5120;

/* Name of the identifier column. */
static char* ID_COL = "identifier";

/* Column holding the identifier. */
static int id_col_nr;

/* Name of the status column. */
static char* STATUS_COL = "csStatus";

/* Column holding the status. */
static int status_col_nr;

/* Name of the DB URL pseudo-column. */
static char* DBURL_COL = "dbUrl";

/* String to use in GET request to require format. */
static char* FORMAT_STR = "format";

/* String to use in GET request to require starting at a given record. */
static char* START_STR = "start";

/* String to use in GET request to require ending at a givenrecord. */
static char* END_STR = "end";

/* Text format directive. */
static char* TEXT_FORMAT_STR = "text";

/* XML format directive. */
static char* XML_FORMAT_STR = "xml";

/* Fields of the jobDefinition table. */
char* fields_str = "";
/* Fields of the jobDefinition table. */
char** fields;
/* Number of fields. */
int cols = 0;

/* Forward declaration */
module AP_MODULE_DECLARE_DATA gridfactory_module;


/**
 * Configuration
 */
 
typedef struct {
  char* ps_;
  char* url_;
} config_rec;

static void*
do_config(apr_pool_t* p, char* d)
{
  config_rec* conf = (config_rec*)apr_pcalloc(p, sizeof(config_rec));
  conf->ps_ = 0;      /* null pointer */
  conf->url_ = 0;      /* null pointer */
  return conf;
}

/**
 * DB stuff
 */

static void*
dbd_prepare(cmd_parms* cmd, void* cfg)
{

    if (dbd_prepare_fn == NULL) {
        dbd_prepare_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_prepare);
        if (dbd_prepare_fn == NULL) {
            return "You must load mod_dbd to use mod_gridfactory";
        }
        dbd_acquire_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_acquire);
    }

    dbd_prepare_fn(cmd->server, JOB_REC_SELECT_PS, LABEL1);
    dbd_prepare_fn(cmd->server, JOB_REC_UPDATE_PS, LABEL2);

    /* save the labels here for our own use */
    ap_set_string_slot(cmd, cfg, LABEL1);
    ap_set_string_slot(cmd, cfg, LABEL2);
}

static const char*
config_ps(cmd_parms* cmd, void* mconfig, const char* arg)
{
  if (((config_rec*)mconfig)->ps_)
    return "PrepareStatements already set.";

  ((config_rec*)mconfig)->ps_ = (char*) arg;
  
  if(apr_strnatcasecmp(((config_rec*)mconfig)->ps_, "On") == 0){
    dbd_prepare(cmd, mconfig);
  }
  
  return 0;
}

static const char*
config_url(cmd_parms* cmd, void* mconfig, const char* arg)
{
  if (((config_rec*)mconfig)->url_)
    return "DBBaseURL already set.";

  ((config_rec*)mconfig)->url_ = (char*) arg;
  
  return 0;
}

static const command_rec command_table[] =
{
    AP_INIT_TAKE1("PrepareStatements", config_ps,
                  NULL, OR_FILEINFO,
                  "Whether or not to use prepared statements."),
    AP_INIT_TAKE1("DBBaseURL", config_url,
                  NULL, OR_FILEINFO,
                  "Base URL of the DB web service."),
    {NULL}
};

unsigned long countchr(const char *str, const char *ch)
{
  unsigned long count;
  for ( ; (*str); ++str ){
    if(*str == *ch){
      ++count;
    }
  }
  return count;
}

/* From apr_dbd_mysql.c */
struct apr_dbd_results_t {
    int random;
    MYSQL_RES *res;
    MYSQL_STMT *statement;
    MYSQL_BIND *bind;
#if APU_MAJOR_VERSION >= 2 || (APU_MAJOR_VERSION == 1 && APU_MINOR_VERSION >= 3)
    apr_pool_t *pool;
#endif
};

int get_fields(request_rec *r, ap_dbd_t* dbd){
  
    if (strcmp(fields_str, "") != 0) {
      return 0;
    }
    
    apr_status_t rv;
    const char* ret = "";
    fields_str = (char *) malloc(MAX_T_F_SIZE * sizeof(char));
    //fields_str = apr_pcalloc(r->pool, 1000); --- doesn't seem to work...
    if(fields_str == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Out of memory.");
      return -1;
    }
      
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t* row = NULL;
    char* val;
    int firstrow = 0;
    int i = 0;
    
    if(apr_dbd_select(dbd->driver, r->pool, dbd->handle, &res, JOB_REC_SHOW_F_Q, 1) != 0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error");
        return -1;
    }
    
    cols = apr_dbd_num_tuples(dbd->driver, res);
    
    fields = malloc(cols * sizeof(char *));
    if(fields == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
        "Out of memory while allocating %i columns.", cols);
      return -1;
    }
    for(i = 0; i < cols; i++){
    fields[i] = malloc(MAX_F_SIZE * sizeof(char));
      if(fields[i] == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Out of memory.");
        return -1;
      }
    }
    
    /* Get the list of fields. */
    i = 0;
    for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
         rv != -1;
         rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
        if(rv != 0){
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
            return -1;
        }
        val = (char*) apr_dbd_get_entry(dbd->driver, row, 0);
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "field --> %s", val);
        if(firstrow != 0){
          ret = apr_pstrcat(r->pool, ret, "\t", NULL);
        }
        ret = apr_pstrcat(r->pool, ret, val, NULL);
        fields[i] = val;
        if(apr_strnatcmp(val, ID_COL) == 0){
          id_col_nr = i;
        }
        else if(apr_strnatcmp(val, STATUS_COL) == 0){
          status_col_nr = i;
        }
        firstrow = -1;
        ++i;
        /* we can't break out here or row won't get cleaned up */
    }
    /* append the pseudo-column 'dbUrl' */
    ret = apr_pstrcat(r->pool, ret, "\t", NULL);
    ret = apr_pstrcat(r->pool, ret, DBURL_COL, NULL);
    
    
    apr_cpystrn(fields_str, ret, strlen(ret)+1);
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Found fields:");
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, fields_str);
    
    //apr_dbd_close(dbd->driver, dbd->handle);
    
    return 0;
}

char* constructURL(request_rec* r, char* job_id){
  config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
  char* uuid = memrchr(job_id, '/', strlen(job_id) - 1) + 1;
  char* id = apr_pstrcat(r->pool, conf->url_, uuid, NULL);
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "ID: %s + %s -> %s", conf->url_, uuid, id);
  return id;
}

char* jobs_text_format(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t *res){
  apr_status_t rv;
  char* val;
  apr_dbd_row_t* row = NULL;
  int i = 0;
  char* dbUrl;
  
  config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
  
  char* recs = fields_str;

  for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
        rv != -1;
        rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
    if (rv != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
        return NULL;
    }
    recs = apr_pstrcat(r->pool, recs, "\n", NULL);
    for (i = 0 ; i < cols ; i++) {
      val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "--> %s", val);
      recs = apr_pstrcat(r->pool, recs, val, NULL);
      if(i == id_col_nr){
        dbUrl = constructURL(r, val);
      }
      recs = apr_pstrcat(r->pool, recs, "\t", NULL);
    }
    /* Append the value for the pseudo-column 'dbUrl'*/
    recs = apr_pstrcat(r->pool, recs, dbUrl, NULL);
    /* we can't break out here or row won't get cleaned up */
  }
  
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning rows");
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, recs);
  // Causes segfalut...
  //free(query);
  //free(dbUrl);
  return recs;
}

char* jobs_xml_format(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t *res){
  apr_status_t rv;
  char* val;
  apr_dbd_row_t* row = NULL;
  int i = 0;
  char* id;
  char* recs;
  
  config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
  
  recs = "<?xml version=\"1.0\"?>\n<jobs>";

  for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
        rv != -1;
        rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
    if (rv != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
        return NULL;
    }
    recs = apr_pstrcat(r->pool, recs, "\n  <job", NULL);
    for (i = 0 ; i < cols ; i++) {
      val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "--> %s", val);
      if(i == id_col_nr){
        id = val;
        recs = apr_pstrcat(r->pool, recs, " ", ID_COL, "=\"", val, "\"", NULL);
      }
      else if(i == status_col_nr){
        recs = apr_pstrcat(r->pool, recs, " ", STATUS_COL, "=\"", val, "\"", NULL);
      }
    }
    recs = apr_pstrcat(r->pool, recs, " ", DBURL_COL, "=\"", constructURL(r, id), "\"/>", NULL);
    /* we can't break out here or row won't get cleaned up */
  }
  recs = apr_pstrcat(r->pool, recs, "\n</jobs> ", NULL);
  
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning rows");
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, recs);
  // Causes segfalut...
  //free(query);
  return recs;
}

char* get_job_recs(request_rec* r, char* args){
    apr_dbd_results_t *res = NULL;
    char* last;
    char* last1;
    char* query = malloc(256 * sizeof(char *));
    char buffer[strlen(args)+1];
    char* token;
    char* subtoken1;
    char* subtoken2;
    /* Output in XML or not. */
    int text = -1;
    int start = -1;
    int end = -1;
    
    snprintf(buffer, sizeof(buffer), args);
    
    // TODO: rest|list
    
    /* GET /grid/db/jobs/?format=text|xml&csStatus=ready|requested|running&... */
    if(r->args && countchr(args, "=") > 0){
      query = JOB_REC_SELECT_Q;
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "args: %s", buffer);
      
      for ((token = strtok_r(buffer, "&", &last)); token;
         token = strtok_r(NULL, "&", &last)) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "token: %s", token);
        subtoken1 = strtok_r(token, "=", &last1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken1: %s", subtoken1);
        if(apr_strnatcmp(subtoken1, FORMAT_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          if(apr_strnatcmp(subtoken2, TEXT_FORMAT_STR) == 0){
            text = 0;
          }
          else if(apr_strnatcmp(subtoken2, XML_FORMAT_STR) == 0){
            text = -1;
          }
          else{
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Format %s unknown.", subtoken2);
            return NULL;
          }
        }
        else if(apr_strnatcmp(subtoken1, START_STR) != 0 && apr_strnatcmp(subtoken1, END_STR) != 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken2: %s", subtoken2);
          query = apr_pstrcat(r->pool, query, " WHERE ", subtoken1, " = '", subtoken2, "'", NULL);
        }
        if(apr_strnatcmp(subtoken1, START_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          start = atoi(subtoken2);
        }
        else if(apr_strnatcmp(subtoken1, END_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          end = atoi(subtoken2);
        }
      }
      if(start > 0 && end < 0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "When specifying 'start' you MUST specify 'end' as well.");
        return NULL;
      }
      else if(start >= 0 && end >= 0){
        query = apr_pstrcat(r->pool, query, " LIMIT ", start, ",", end - start +1, NULL);
      }
      else if(start < 0 && end >= 0){
        query = apr_pstrcat(r->pool, query, " LIMIT ", end - start +1, NULL);
      }
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query: %s", query);
    }
    /* GET /grid/db/jobs/ */
    else{
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "GET with no args");
    }

    ap_dbd_t* dbd = dbd_acquire_fn(r);
    if (dbd == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return NULL;
    }
    if (get_fields(r, dbd) < 0) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to get fields.");
      return NULL;
    }
    if (apr_dbd_select(dbd->driver, r->pool, dbd->handle, &res, query, 1) != 0) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error");
      return NULL;
    }
    
    if(text == 0){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning text");
      return jobs_text_format(r, dbd, res);
    }
    else{
      return jobs_xml_format(r, dbd, res);
    }

}

char* get_job_rec_ps(request_rec *r, char* uuid){
    apr_status_t rv;
    apr_dbd_prepared_t *statement;
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t *row = NULL;
    char* val;
    int i;
    char* rec = 0;
    int firstrow = 0;
    config_rec* conf;
    
    ap_dbd_t* dbd = dbd_acquire_fn(r);
    if(dbd == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return NULL;
    }
    
    statement = apr_hash_get(dbd->prepared, LABEL1, APR_HASH_KEY_STRING);
    if (statement == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "A prepared statement could not be found for getting job records.");
        return NULL;
    }
    
    if (apr_dbd_pvselect(dbd->driver, r->pool, dbd->handle, &res, statement,
                              1, uuid, NULL) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Query execution error looking up '%s' "
                      "in database", uuid);
        return NULL;
    }
    
    for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
         rv != -1;
         rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
      if (rv != 0) {
         ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
          return NULL;
      }
      if(firstrow != 0){
        rec = apr_pstrcat(r->pool, rec, "\n\n", NULL);
      }   
      for (i = 0 ; i < cols ; i++) {
        val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "--> %s", val);
        if(i > 0){
          rec = apr_pstrcat(r->pool, rec, "\n", NULL);
        }
        rec = apr_pstrcat(r->pool, rec, val, NULL);
      }
      firstrow = -1;
      /* we can't break out here or row won't get cleaned up */
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning record");
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, rec);
    return rec;
}

char* get_job_rec_s(request_rec *r, char* uuid){
  // TODO
  return NULL;
}

char* get_job_rec(request_rec *r, char* uuid){
    config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    if(conf->ps_ == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PrepareStatements not specified");
      return get_job_rec_s(r, uuid);
    }
    else{
      return get_job_rec_ps(r, uuid);
    }  
} 

char* update_job_rec(request_rec *r, char* id) {
} 

static int gridfactory_db_handler(request_rec *r) {
    char* job_uuid;
    config_rec* conf;
    int uri_len = strlen(r->uri);
    int jobdir_len = strlen(JOB_DIR);
    int ok = 0;
    char* response = "";
    char* base_path = malloc(128 * sizeof(char *));
    char* path_end;
    char* base_url;
    
    conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "handler %s", r->handler);

    if (!r->handler || strcmp(r->handler, "gridfactory"))
      return DECLINED;
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "URI: %s", r->uri);

    /* If DBBaseURL was not set in the preferences, default to this server. */
    if(conf->url_ == NULL){
      conf->url_ = (char*)apr_pcalloc(r->pool, 256);
      base_url = apr_pstrcat(r->pool, "https://", r->server->server_hostname, NULL);
      //conf->url_ = base_url;
      apr_cpystrn(conf->url_, base_url, strlen(base_url) + 1);
      if(r->server->port && r->server->port != 443){
        conf->url_ = apr_pstrcat(r->pool, conf->url_, ":", apr_itoa(r->pool, r->server->port), NULL);
      }
      apr_cpystrn(base_path, r->uri, uri_len + 1);
      path_end = strstr(base_path, JOB_DIR);
      if(path_end != NULL){
        apr_cpystrn(base_path, r->uri , uri_len - strlen(path_end) + 1);
        conf->url_ = apr_pstrcat(r->pool, conf->url_, base_path, JOB_DIR, NULL);
      }
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "URL not set, defaulting to %s, %s, %s, %s",
        conf->url_, path_end, base_path, r->uri);
    }
    else{
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "URL set.");
    }

    /* GET */
    if (r->method_number == M_GET) {
      if(apr_strnatcmp((r->uri) + uri_len - jobdir_len, JOB_DIR) == 0){
        /* GET /grid/db/jobs/?... */
        response = apr_pstrcat(r->pool, response, get_job_recs(r, r->args), NULL);
      }    
      /* GET /grid/db/jobs/UUID */
      else if(uri_len > jobdir_len) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Getting job record from DB %s", (r->uri) + uri_len - jobdir_len);
        /* job_uuid = "UUID" */
        job_uuid = memrchr(r->uri, '/', uri_len);
        apr_cpystrn(job_uuid, job_uuid+1 , uri_len - 1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "job_uuid --> %s", job_uuid);
        response = apr_pstrcat(r->pool, response, get_job_rec(r, job_uuid), NULL);
      }
      else{
        ok = -1;
      }
    }
    /* PUT /grid/db/jobs/UUID */
    else if (r->method_number == M_PUT) {
      if(uri_len > jobdir_len) {
        job_uuid = memrchr(r->uri, '/', uri_len);
        apr_cpystrn(job_uuid, job_uuid+1 , uri_len - 1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "job_uuid --> %s", job_uuid);
      }
      else{
        ok = -1;
      }
    }
    else{
      ok = -1;
    }
    free(base_path);
    if(ok < 0){
      r->allowed = (apr_int64_t) ((1 < M_GET) | (1 < M_PUT));
      return DECLINED;
    }
    else{
      /*ap_set_content_type(r, "text/html;charset=ascii");
      ap_rputs("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\">\n", r);
      ap_rputs("<html><head><title>Hello World!</title></head>", r);
      ap_rputs("<body><h1>Hello World!</h1>", r);*/
      ap_set_content_type(r, "text/plain;charset=ascii");
      ap_rputs(response, r);
      /*ap_rputs("</body></html>", r);*/
    }
    
    return OK;
}

static void register_hooks(apr_pool_t *p)
{
  ap_hook_handler(gridfactory_db_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA gridfactory_module =
{
    STANDARD20_MODULE_STUFF,
    do_config,
    NULL,
    NULL,
    NULL,
    command_table,
    register_hooks
};

