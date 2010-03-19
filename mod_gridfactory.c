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
 * This module allows you to create a GridFactory web service for job pulling.
 * It requires mod_dbm to be loaded and configured.
 * 
 * Load the module with:
 * 
 *  LoadModule gridfactory_module /usr/lib/apache2/modules/mod_gridfactory.so
 *  <Location /db>
 *    SetHandler gridfactory
 *  </Location>
 * 
 * The directory "db" should be a symlink to /var/spool/gridfactory.
 * 
 * Two configuration directives are available:
 * 
 *   DBBaseURL "URL"
 *      The URL served with this module. If this is not specified,
 *      "https://my.host.hame/db/jobs/" is used.
 * 
 *   PrepareStatements "On|Off"
 *      Whether or not MySQL prepared statements should be used. I don't
 *      really see any reason to set this to Off.
 */

#include "ap_provider.h"
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
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
static char* LABEL = "gridfactory_dbd_0";
static char* LABEL1 = "gridfactory_dbd_1";
static char* LABEL2 = "gridfactory_dbd_2";
static char* LABEL3 = "gridfactory_dbd_3";

/* Name of the identifier column. */
static char* ID_COL = "identifier";

/* Column holding the identifier. */
static int id_col_nr;

/* Name of the status column. */
static char* STATUS_COL = "csStatus";

/* ready value of the status column. */
static char* READY = "ready";

/* Name of the lastModified column. */
static char* LASTMODIFIED_COL = "lastModified";

/* Name of the providerInfo column. */
static char* PROVIDERINFO_COL = "providerInfo";


/* Column holding the status. */
static int status_col_nr;

/* Name of the DB URL pseudo-column. */
static char* DBURL_COL = "dbUrl";

/* SQL query to get list of fields. */
static char* JOB_REC_SHOW_F_Q = "SHOW fields FROM `jobDefinition`";

/* SQL query to get all job records. */
static char* JOB_RECS_SELECT_Q = "SELECT * FROM `jobDefinition`";

/* Prepared statement string to get job record. */
static char* JOB_REC_SELECT_PS = "SELECT * FROM jobDefinition WHERE identifier LIKE ?";

/* Query to get job record. */
static char* JOB_REC_SELECT_Q = "SELECT * FROM `jobDefinition` WHERE identifier LIKE '%/%s'";

/* Prepared statement string to update job record. */
static char* JOB_REC_UPDATE_PS_1 = "UPDATE `jobDefinition` SET lastModified = NOW() WHERE identifier LIKE ?";

/* Prepared statement string to update job record. */
static char* JOB_REC_UPDATE_PS_2 = "UPDATE `jobDefinition` SET lastModified = NOW(), csStatus = ? WHERE identifier LIKE ?";

/* Prepared statement string to update job record. */
static char* JOB_REC_UPDATE_PS_3 = "UPDATE `jobDefinition` SET lastModified = NOW(), csStatus = ?, providerInfo = ? WHERE identifier LIKE ?";

/* Query to update job record. */
static char* JOB_REC_UPDATE_Q = "UPDATE `jobDefinition` SET lastModified = NOW()";

/* Optional function - look it up once in post_config. */
static ap_dbd_t* (*dbd_acquire_fn)(request_rec*) = NULL;
static void (*dbd_prepare_fn)(server_rec*, const char*, const char*) = NULL;

/* Max size of each field name. */
static int MAX_F_SIZE = 256;

/* Max size of all field names. */
static int MAX_T_F_SIZE = 5120;

/* Max size (bytes) of response and PUT bodies. */
static int MAX_SIZE = 100000;

/* Whether or not to operate in private mode (1 = private). */
static int PRIVATE = 1;

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

/* Public fields of the jobDefinition table. */
char* pub_fields_str = "identifier\tname\tcsStatus\tuserInfo\tcreated\tlastModified\tgridTime\tmemory\topSys\truntimeEnvironments\tallowedVOs\tvirtualize\tdbUrl";
/* Public fields of the jobDefinition table. */
char** pub_fields;
/* Number of public fields. */
int pub_cols = 0;

/* Base URL for the DB web service. */
char* base_url;

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
  dbd_acquire_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_acquire);
  if(dbd_acquire_fn == NULL){
    dbd_acquire_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_acquire);
    if(dbd_acquire_fn == NULL){
        return "You must load mod_dbd to use mod_gridfactory";
    }
  }
  dbd_prepare_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_prepare);
  config_rec* conf = (config_rec*)apr_pcalloc(p, sizeof(config_rec));
  conf->ps_ = 0;      /* null pointer */
  conf->url_ = 0;      /* null pointer */
  return conf;
}

/**
 * DB stuff
 */

static void
dbd_prepare(cmd_parms* cmd, void* cfg)
{
    dbd_prepare_fn(cmd->server, JOB_REC_SELECT_PS, LABEL);
    dbd_prepare_fn(cmd->server, JOB_REC_UPDATE_PS_1, LABEL1);
    dbd_prepare_fn(cmd->server, JOB_REC_UPDATE_PS_2, LABEL2);
    dbd_prepare_fn(cmd->server, JOB_REC_UPDATE_PS_3, LABEL3);
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
  unsigned long count = 0;
  for ( ; (*str); ++str ){
    if(*str == *ch){
      ++count;
    }
  }
  return count;
}

/* From apr_dbd_mysql.c */
/*struct apr_dbd_results_t {
    int random;
    MYSQL_RES *res;
    MYSQL_STMT *statement;
    MYSQL_BIND *bind;
#if APU_MAJOR_VERSION >= 2 || (APU_MAJOR_VERSION == 1 && APU_MINOR_VERSION >= 3)
    apr_pool_t *pool;
#endif
};*/

typedef struct {
    int format;
    char* res;
    /* Used only by update_job_rec, to check if a job is
     * "ready" before allowing writing. If a job is "ready"
     * only writing the status (csStatus) is allowed. */
    char* status;
} db_result;

int set_pub_fields(request_rec *r){

  if (pub_cols == 0) {
    return 0;
  }
  
  char* field;
  const char* delim = "\t";
  char* last;
  int i = 0;
  
  /* Split the fields on " " */
  for(field = apr_strtok(pub_fields_str, delim, &last); field != NULL;
      field = apr_strtok(NULL, delim, &last)){
    pub_fields[i] = malloc(MAX_F_SIZE * sizeof(char));
    if(pub_fields[i] == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Out of memory.");
      return -1;
    }
    apr_cpystrn(pub_fields[i], field, strlen(field)+1);
    ++i;
  }
  pub_cols = i;
  return 0;
}

int set_fields(request_rec *r, ap_dbd_t* dbd){
  
    if (strcmp(fields_str, "") != 0) {
      return 0;
    }
    
    apr_status_t rv;
    const char* ret = "";
    // Hmm, does not work. Memory gets overwritten...
    //fields_str = apr_pcalloc(r->pool, MAX_T_F_SIZE * sizeof(char));
    fields_str = (char*) malloc(MAX_T_F_SIZE * sizeof(char*));
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
    
    fields = malloc(cols * sizeof(char*));
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
        apr_cpystrn(fields[i], val, strlen(val)+1);
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
    
    return 0;
}

char* constructURL(request_rec* r, char* job_id){
  char* uuid = memrchr(job_id, '/', strlen(job_id) - 1) + 1;
  char* id = apr_pstrcat(r->pool, base_url, uuid, NULL);
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "DB URL: %s + %s -> %s", base_url, uuid, id);
  return id;
}

char* jobs_text_format(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t *res, int priv){
  apr_status_t rv;
  char* val;
  apr_dbd_row_t* row = NULL;
  int i = 0;
  char* dbUrl = "";
  char* recs;
  char* checkStr;
  char* checkStart;
  char* checkEnd;
  int fieldLen;
  
  if(priv){
    recs = pub_fields_str;
  }
  else{
    recs = fields_str;
  }

  for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
        rv != -1;
        rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
    if (rv != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
        return NULL;
    }
    recs = apr_pstrcat(r->pool, recs, "\n", NULL);
    for (i = 0 ; i < cols ; i++) {
      
      // To check if a field is a member of pub_fields, just see if pub_fields_str contains
      // " field ".
      checkStr = strstr(pub_fields_str, fields[i]);
      fieldLen = strlen(fields[i]);
      checkStart = checkStr-1;
      checkEnd = checkStr+fieldLen;
      if(priv && (
        checkStr == NULL ||
        *checkStart != 0 && *checkStart != ' ' && *checkStart != '\t' ||
        *checkEnd != 0 && *checkEnd != ' ' && *checkEnd != '\t'
        )){
        continue;
      }  
      
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

  return recs;
}

char* jobs_xml_format(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t *res, int priv){
  apr_status_t rv;
  char* val;
  apr_dbd_row_t* row = NULL;
  int i = 0;
  char* id = "";
  char* recs;
  
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

  return recs;
}

/**
 * Appends text representing DB result to 'recs'.
 * Returns 0 if text output was requested or no format was specified.
 * Setting the switch 'priv' to 1 turns on privacy. Privacy means that only the fields of
 * 'pub_fields' are shown.
 */
db_result* get_job_recs(request_rec* r, db_result* ret, int priv){
    apr_dbd_results_t *res = NULL;
    char* last;
    char* last1;
    char* query = (char*)apr_pcalloc(r->pool, 256 * sizeof(char*));
    char* token;
    char* subtoken1;
    char* subtoken2;
    int start = -1;
    int end = -1;
    ret->format = 0;
    
    snprintf(query, strlen(JOB_RECS_SELECT_Q)+1, JOB_RECS_SELECT_Q);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query0: %s", query);
    /* GET /grid/db/jobs/?format=text|xml&csStatus=ready|requested|running&... */
    if(r->args && countchr(r->args, "=") > 0){
      char buffer[strlen(r->args)+1];
      snprintf(buffer, strlen(r->args)+1, r->args);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "args: %s", buffer);
      
      for ((token = strtok_r(buffer, "&", &last)); token;
         token = strtok_r(NULL, "&", &last)) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "token: %s", token);
        subtoken1 = strtok_r(token, "=", &last1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken1: %s", subtoken1);
        if(apr_strnatcmp(subtoken1, FORMAT_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          if(apr_strnatcmp(subtoken2, TEXT_FORMAT_STR) == 0){
            ret->format = 0;
          }
          else if(apr_strnatcmp(subtoken2, XML_FORMAT_STR) == 0){
            ret->format = 1;
          }
          else{
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Format %s unknown.", subtoken2);
            //return NULL;
          }
        }
        else if(apr_strnatcmp(subtoken1, START_STR) != 0 && apr_strnatcmp(subtoken1, END_STR) != 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken2: %s", subtoken2);
          query = apr_pstrcat(r->pool, query, " WHERE ", subtoken1, " = '", subtoken2, "'", NULL);
        }
        else if(apr_strnatcmp(subtoken1, START_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken2: %s", subtoken2);
          start = atoi(subtoken2);
        }
        else if(apr_strnatcmp(subtoken1, END_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken2: %s", subtoken2);
          end = atoi(subtoken2);
        }
      }
      if(start > 0 && end < 0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "When specifying 'start' you MUST specify 'end' as well.");
        return NULL;
      }
      else if(start >= 0 && end >= 0){
        query = apr_pstrcat(r->pool, query, " LIMIT ", apr_itoa(r->pool, start), ",", apr_itoa(r->pool, end - start +1), NULL);
      }
      else if(start < 0 && end >= 0){
        query = apr_pstrcat(r->pool, query, " LIMIT ", apr_itoa(r->pool, end +1), NULL);
      }
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query: %s", query);
    }
    /* GET /grid/db/jobs/ */
    else{
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "GET with no args");
    }

    ap_dbd_t* dbd = dbd_acquire_fn(r);
    if(dbd == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return NULL;
    }

    if(priv && set_pub_fields(r) < 0){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to set public fields.");
      return NULL;
    }
    
    if(set_fields(r, dbd) < 0){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to set fields.");
      return NULL;
    }
    
    if(apr_dbd_select(dbd->driver, r->pool, dbd->handle, &res, query, 1) != 0){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error");
      return NULL;
    }
    
    if(ret->format == 0){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning text");
      ret->res = jobs_text_format(r, dbd, res, priv);
    }
    else if(ret->format == 1){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning XML");
      ret->res = jobs_xml_format(r, dbd, res, priv);
    }
    
    // Dont't do this. It causes segfaults...
    //apr_dbd_close(dbd->driver, dbd->handle);
    
    return ret;
}

void get_job_rec_ps(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t* res,
   char* uuid){

    apr_dbd_prepared_t* statement = apr_hash_get(dbd->prepared, LABEL, APR_HASH_KEY_STRING);
    if(statement == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "A prepared statement could not be found for getting job records.");
    }
    
    char* str = apr_pstrcat(r->pool, "%/", uuid, NULL);
    if(apr_dbd_pvselect(dbd->driver, r->pool, dbd->handle, &res, statement,
                              0, str, NULL) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Query execution error looking up '%s' in database", uuid);
    }
}

void get_job_rec_s(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t* res,
   char* uuid){
  char* query = (char*)apr_pcalloc(r->pool, 256 * sizeof(char*));
  snprintf(query, strlen(JOB_REC_SELECT_Q)+strlen(uuid)-1, JOB_REC_SELECT_Q, uuid);
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query: %s", query);
  if(apr_dbd_select(dbd->driver, r->pool, dbd->handle, &res, query, 1) != 0){
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error");
  }
}

void job_text_format(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t* res, db_result* ret){

    apr_dbd_row_t* row = NULL;
    apr_status_t rv;
    char* val;
    int i;
    int firstrow = 0;
    char* rec = "";

    for(rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
         rv != -1;
         rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
      if(rv != 0){
         ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
          return;
      }
      if(firstrow != 0){
        rec = apr_pstrcat(r->pool, rec, "\n\n", NULL);
      }
      for(i = 0 ; i < cols ; i++){
        val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "--> %s", val);
        if(i > 0){
          rec = apr_pstrcat(r->pool, rec, "\n", NULL);
        }
        rec = apr_pstrcat(r->pool, rec, fields[i], ": ", val, NULL);
        // Set res.status
        if(strcmp(fields[i], STATUS_COL) == 0 && val != NULL){
        	ret->status = val;
        }
      }
      firstrow = -1;
      /* we can't break out here or row won't get cleaned up */
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning record:");
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, rec);
    
    ret->res = rec;
}

void job_xml_format(request_rec *r, ap_dbd_t* dbd, apr_dbd_results_t* res, db_result* ret){

    apr_dbd_row_t* row = NULL;
    apr_status_t rv;
    char* val;
    int i;
    int firstrow = 0;
    char* rec = "<?xml version=\"1.0\"?>\n<job>";

    for(rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
         rv != -1;
         rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
      if(rv != 0){
         ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
          return;
      }
      if(firstrow != 0){
        rec = apr_pstrcat(r->pool, rec, "\n\n", NULL);
      }
      for(i = 0 ; i < cols ; i++){
        val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "--> %s", val);
        if(val && strcmp(val, "") != 0){
          rec = apr_pstrcat(r->pool, rec, "\n  <", fields[i], ">", val,
             "</", fields[i], ">", NULL);
        }
        // Set res.status
        if(strcmp(fields[i], STATUS_COL) == 0 && val != NULL){
        	ret->status = val;
        }
      }
      firstrow = -1;
      /* we can't break out here or row won't get cleaned up */
    }
    rec = apr_pstrcat(r->pool, rec, "\n</job> ", NULL);
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning record:");
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, rec);
    
    ret->res = rec;

}

void get_job_rec(request_rec *r, char* uuid, db_result* ret){
  
    char* token;
    char* subtoken1;
    char* subtoken2;
    char* last;
    char* last1;
    ret->format = 0;

    apr_dbd_results_t* dbres = (apr_dbd_results_t*)apr_pcalloc(r->pool, 5120);  
    //apr_dbd_results_t* res = NULL;
  
    ap_dbd_t* dbd = dbd_acquire_fn(r);
    if(dbd == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return;
    }
    
    if(set_fields(r, dbd) < 0){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to get fields.");
      return;
    }
    
    config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    if(conf->ps_ == NULL ||
      apr_strnatcasecmp(conf->ps_, "On") != 0){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PrepareStatements not enabled, %s.", conf->ps_);
      get_job_rec_s(r, dbd, dbres, uuid);
    }
    else{
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PrepareStatements enabled, %s.", conf->ps_);
      get_job_rec_ps(r, dbd, dbres, uuid);
    }
    
    if(dbres == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Nothing returned from query.");
      return;
    }
    
    if(r->args && countchr(r->args, "=") > 0){
      char buffer[strlen(r->args)+1];
      snprintf(buffer, strlen(r->args)+1, r->args);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "args: %s", buffer);
      
      for ((token = strtok_r(buffer, "&", &last)); token;
         token = strtok_r(NULL, "&", &last)) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "token: %s", token);
        subtoken1 = strtok_r(token, "=", &last1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken1: %s", subtoken1);
        if(apr_strnatcmp(subtoken1, FORMAT_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          if(apr_strnatcmp(subtoken2, TEXT_FORMAT_STR) == 0){
            ret->format = 0;
          }
          else if(apr_strnatcmp(subtoken2, XML_FORMAT_STR) == 0){
            ret->format = 1;
          }
          else{
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Format %s unknown.", subtoken2);
            //return NULL;
          }
        }
      }
    }
    if(ret->format == 0){
      job_text_format(r, dbd, dbres, ret);
    }
    else if(ret->format == 1){
      job_xml_format(r, dbd, dbres, ret);
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Returning:");
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, ret->res);
    
    // Dont't do this. It causes segfaults...
    //apr_dbd_close(dbd->driver, dbd->handle);

}

/* From http://www.wilsonmar.com/1strings.htm. */
void ltrim( char * string, char * trim )
{
  while ( string[0] != '\0' && strchr ( trim, string[0] ) != NULL )
  {
    memmove( &string[0], &string[1], strlen(string) );
  }
}

/* From The Apache Modules Book. */
/* Parse PUT text data from a string. The input string is NOT preserved. */
static apr_hash_t* parse_put_from_string(request_rec* r, char* args){
  apr_hash_t* tbl;
  char* pair;
  char* eq;
  const char* delim = "\n";
  const char* sep = ":";
  char* last;
  
  if(args == NULL){
    return NULL;
  }
  
  tbl = apr_hash_make(r->pool);
  
  /* Split the input on "\n" */
  for(pair = apr_strtok(args, delim, &last); pair != NULL;
      pair = apr_strtok(NULL, delim, &last)){
    for(eq = pair; *eq; ++eq){
      if(*eq == '+'){
        *eq = ' ';
      }
    }
    eq = strstr(pair, sep);
    if(eq){
      *eq++ = '\0';
      ltrim(eq, " ");
      ap_unescape_url(eq);
    }
    else{
      eq = "";
    }
    ltrim(pair, " ");
    ap_unescape_url(pair);
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "key: %s", pair);
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "value: %s", eq);
    apr_hash_set(tbl, pair, APR_HASH_KEY_STRING, apr_pstrdup(r->pool, eq));
    
  }
  return tbl;
}

/* From The Apache Modules Book. */
static int parse_input_from_put(request_rec* r, apr_hash_t **form){
  int bytes, eos;
  apr_size_t count;
  apr_status_t rv;
  apr_bucket_brigade* bb;
  apr_bucket_brigade* bbin;
  char* buf;
  apr_bucket* b;
  
  const char* clen = apr_table_get(r->headers_in, "Content-Length");
  if(clen != NULL){
    bytes = strtol(clen, NULL, 0);
    if(bytes >= MAX_SIZE){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Request too big (%d bytes; limit %d).", bytes, MAX_SIZE);
      return HTTP_REQUEST_ENTITY_TOO_LARGE;
    }
  }
  else{
    bytes = MAX_SIZE;
  }
  
  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Buffer size: %d bytes", bytes);
  
  bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
  bbin = apr_brigade_create(r->pool, r->connection->bucket_alloc);
  count = 0;
  
  do{
    rv = ap_get_brigade(r->input_filters, bbin, AP_MODE_READBYTES, APR_BLOCK_READ, bytes);
    if(rv != APR_SUCCESS){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to read PUT input.");
      return HTTP_INTERNAL_SERVER_ERROR;
    }
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Looping");
    for(b = APR_BRIGADE_FIRST(bbin);
       b != APR_BRIGADE_SENTINEL(bbin);
       b = APR_BUCKET_NEXT(b)){
      if(APR_BUCKET_IS_EOS(b) /* had to add this to avoid infinite loop: */|| b->length <= 0 || APR_BUCKET_IS_METADATA(b)){
        eos = 1;
        break;
      }
      if(!APR_BUCKET_IS_METADATA(b)){
        if(b->length != (apr_size_t)(-1)){
          count += b->length;
          if(count > MAX_SIZE){
            apr_bucket_delete(b);
          }
        }
      }
      if(count <= MAX_SIZE){
        APR_BUCKET_REMOVE(b);
        APR_BRIGADE_INSERT_TAIL(bb, b);
      }
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Read %d", count);
    }
  } while (!eos);
  
  /* Done with the data. Kill the request if we got too much. */
  if(count > MAX_SIZE){
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Request too big (%d bytes; limit %d).", bytes, MAX_SIZE);
    return HTTP_REQUEST_ENTITY_TOO_LARGE;
  }
  
  /* Put the data in a buffer and parse it. */
  buf = apr_palloc(r->pool, count + 1);
  rv = apr_brigade_flatten(bb, buf, &count);
  if(rv != APR_SUCCESS){
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Error (flatten) reading form data.");
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  buf[count] = '\0';
  *form = parse_put_from_string(r, buf);
  
  return OK;
  
}

int update_job_rec(request_rec *r, char* uuid) {
  
  /* Read and parse the data. */
  apr_hash_t* put_data = NULL;
  int status = parse_input_from_put(r, &put_data);
  if(status != OK){
     ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Error reading request body.");
     return status;
  }
  
  /* Now update the database record. */
  apr_hash_index_t* index;
  char* query = "";
  const char* provider = NULL;
  int nrows;
  /* 0 -> no fields to be updated (except for lastModified),
     1 -> only csStatus or providerInfo to be updated,
     2 -> only csStatus and providerInfo to be updated,
     3 -> other than csStatus and providerInfo to be updated. */
  int status_only = 0;
  
  query = apr_pstrcat(r->pool, query, JOB_REC_UPDATE_Q, NULL);
  for(index = apr_hash_first(NULL, put_data);
     index; index = apr_hash_next(index)){
    const char *k;
    const char *v;
    apr_hash_this(index, (const void**)&k, NULL, (void**)&v);
    if(status_only < 2 && (apr_strnatcmp(k, STATUS_COL) == 0 ||
       apr_strnatcmp(k, PROVIDERINFO_COL) == 0)){
      status_only = status_only + 1;
    }
    if(apr_strnatcmp(k, PROVIDERINFO_COL) == 0 ){
      provider = v;
    }
    if(apr_strnatcmp(k, LASTMODIFIED_COL) != 0 &&
       apr_strnatcmp(k, STATUS_COL) != 0 &&
       apr_strnatcmp(k, PROVIDERINFO_COL) != 0){
      status_only = 3;
    }
    if(apr_strnatcmp(k, LASTMODIFIED_COL) != 0){
      query = apr_pstrcat(r->pool, query, ", ", ap_escape_html(r->pool, k), " = '",
                        ap_escape_html(r->pool, v), "'", NULL);
    }
  }
  
  /* If someone is trying to update other fields than csStatus or
     changing only lastModified, check if the job status starts with 'ready';
     if it does, decline. */
  if(status_only != 1 && status_only != 2){
  	db_result ret = {0, "", ""};
    get_job_rec(r, uuid, &ret);
    if(ret.status && strstr(READY, ret.status) != NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
         "For %s jobs, only changing csStatus and providerInfo is allowed. %s --> %s",
         ret.status, status_only, query);
      return DECLINED;
    }
  }

  config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
  ap_dbd_t* dbd = dbd_acquire_fn(r);
  if(dbd == NULL){
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  
  if(conf->ps_ != NULL && apr_strnatcasecmp(conf->ps_, "On") == 0 &&
     status_only == 0){
    /* If no key is present, use prepared statement. */
    apr_dbd_prepared_t* statement = apr_hash_get(dbd->prepared, LABEL1, APR_HASH_KEY_STRING);
    if(statement == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "A prepared statement could not be found for putting job records.");
    }
    char* str = apr_pstrcat(r->pool, "%/", uuid, NULL);
    if(apr_dbd_pvquery(dbd->driver, r->pool, dbd->handle, &nrows,
       statement, str) != 0) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error for %s.", str);
    }
  }
  else if(conf->ps_ != NULL && apr_strnatcasecmp(conf->ps_, "On") == 0 &&
          status_only == 1 && provider == NULL){
    /* If only the csStatus key is present, use prepared statement. */
    apr_dbd_prepared_t* statement = apr_hash_get(dbd->prepared, LABEL2, APR_HASH_KEY_STRING);
    if(statement == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "A prepared statement could not be found for putting job records.");
    }
    char* str = apr_pstrcat(r->pool, "%/", uuid, NULL);
    if(apr_dbd_pvquery(dbd->driver, r->pool, dbd->handle, &nrows,
       statement, status, str) != 0) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error for %i, %s", status, str);
    }
  }
  else if(conf->ps_ != NULL && apr_strnatcasecmp(conf->ps_, "On") == 0 &&
          (status_only == 1 || status_only == 2) && provider != NULL){
    /* If only the csStatus and providerInfo keys are present, use prepared statement. */
    apr_dbd_prepared_t* statement = apr_hash_get(dbd->prepared, LABEL3, APR_HASH_KEY_STRING);
    if(statement == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "A prepared statement could not be found for putting job records.");
    }
    char* str = apr_pstrcat(r->pool, "%/", uuid, NULL);
    if(apr_dbd_pvquery(dbd->driver, r->pool, dbd->handle, &nrows,
       statement, status, provider, str) != 0) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error for %i, %s, %s", status, provider, str);
    }
  }
  else{
    /* Otherwise, just use a normal query. */
    query = apr_pstrcat(r->pool, query, " WHERE ", ID_COL, " LIKE '%/", uuid, "'", NULL);  
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query: %s", query);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Provider: %s", provider);
    if(apr_dbd_query(dbd->driver, dbd->handle, &nrows, query) != 0){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error");
      return HTTP_INTERNAL_SERVER_ERROR;
    }    
  }
  
  /* If we return HTTP_CREATED, Apache spits out:

     <p>The server encountered an internal error or
     misconfiguration and was unable to complete
     your request.</p>
     <p>Please contact the server administrator,
     [no address given] and inform them of the time the error occurred,
     and anything you might have done that may have
     caused the error.</p>
     <p>More information about this error may be available
     in the server error log.</p>
 
   */

  /*
  if(nrows > 0){
    return HTTP_CREATED;
  }
  else{
    return OK;
  }
  */
  
  return OK;
  
}

/* From mod_ftpd. */
/* Prevent mod_dir from adding directory indexes */
static int fixup_path(request_rec *r)
{
  if(r->handler && strcmp(r->handler, "gridfactory") == 0){
    r->path_info = r->filename;
  }
  return OK;
}

static int gridfactory_db_handler(request_rec *r) {
    char* job_uuid;
    config_rec* conf;
    int uri_len = 0;
    if (r->uri != NULL) uri_len = strlen(r->uri);
    int jobdir_len = strlen(JOB_DIR);
    int ok = OK;
    char* base_path;
    char* tmp_url;
    char* path_end;
    db_result ret = {0, "", ""};
    
    if (r->per_dir_config == NULL) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "directory for mod_gridfactory is null. Maybe your config is missing a Directory directive.");
      return DECLINED;      
    }    
    conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "handler %s", r->handler);

    if (!r->handler || strcmp(r->handler, "gridfactory"))
      return DECLINED;
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "URI: %s", r->uri);

    /* If DBBaseURL was not set in the preferences, default to this server. */
    if(conf->url_ == NULL || strcmp(conf->url_, "") == 0){
      base_url = (char*)apr_pcalloc(r->pool, sizeof(char*) * 256);
      base_path = (char*)apr_pcalloc(r->pool, sizeof(char*) * 256);
      tmp_url = apr_pstrcat(r->pool, "https://", r->server->server_hostname, NULL);
      if(r->server->port && r->server->port != 443){
        tmp_url = apr_pstrcat(r->pool, tmp_url, ":", apr_itoa(r->pool, r->server->port), NULL);
      }
      apr_cpystrn(base_path, r->uri, uri_len + 1);
      path_end = strstr(base_path, JOB_DIR);
      if(path_end != NULL){
        apr_cpystrn(base_path, base_path , uri_len - strlen(path_end) + 1);
        tmp_url = apr_pstrcat(r->pool, tmp_url, base_path, JOB_DIR, NULL);
      }
      apr_cpystrn(base_url, tmp_url, strlen(tmp_url)+1);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "DB base URL not set, defaulting to %s, %s, %s, %s",
        base_url, path_end, base_path, r->uri);
    }
    else{
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "DB base URL set to %s.", conf->url_);
      apr_cpystrn(base_url, conf->url_, strlen(conf->url_)+1);
    }

    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Request: %s", r->the_request);
    /* GET */
    if(r->method_number == M_GET){
      if(apr_strnatcmp((r->uri) + uri_len - jobdir_len, JOB_DIR) == 0){
        /* GET /grid/db/jobs/?... */
        get_job_recs(r, &ret, PRIVATE);
      }    
      /* GET /grid/db/jobs/UUID */
      else if(uri_len > jobdir_len) {
        /* job_uuid = "UUID" */
        /* Chop off any trailing / */
        if (((r->uri)[(strlen(r->uri)-1)]) == '/') {
          (r->uri)[(strlen(r->uri)-1)] = 0;
        }
        job_uuid = memrchr(r->uri, '/', uri_len);
        apr_cpystrn(job_uuid, job_uuid+1 , uri_len - 1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "job_uuid --> %s", job_uuid);
        get_job_rec(r, job_uuid, &ret);
      }
      else{
        return DECLINED;
      }
      
      if(ret.format == 0){
        ap_set_content_type(r, "text/plain;charset=ascii");
        ap_rputs(ret.res, r);
      }
      else if(ret.format == 1){
        ap_set_content_type(r, "text/xml;charset=ascii");
        ap_rputs(ret.res, r);
      }
      else{
        return DECLINED;
      } 
    }
    /* PUT /grid/db/jobs/UUID */
    /*
     * Test with e.g.
     * curl --insecure --cert /home/fjob/.globus/usercert.pem --key /home/fjob/.globus/userkey.pem \
     * --upload-file 3a86aacc-2d5f-11dd-80f2-c3b981785945 \
     * https://localhost/db/jobs/3a86aacc-2d5f-11dd-80f2-c3b981785945
     */
    else if(r->method_number == M_PUT){
      char* tmpstr = (char*)apr_pcalloc(r->pool, sizeof(char*) * 256);
      apr_cpystrn(tmpstr, strstr(r->uri, JOB_DIR), jobdir_len + 1);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PUT %s", r->uri);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Check: %s <-> %s", tmpstr, JOB_DIR);
      if(strcmp(JOB_DIR, tmpstr) == 0) {
        job_uuid = memrchr(r->uri, '/', uri_len);
        apr_cpystrn(job_uuid, job_uuid+1 , uri_len - 1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "job_uuid --> %s", job_uuid);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Content type: %s", r->content_type);
        ok = update_job_rec(r, job_uuid);
      }
      else{
        return DECLINED;
      }
    }
    else{
      r->allowed = (apr_int64_t) ((1 < M_GET) | (1 < M_PUT));
    }
    
    /* Causes segfaults... */
    /*int i;
    for(i = 0; i < cols; i++){
      free(fields[i]);
    }
    free(fields);
    free(fields_str);*/

    return ok;
}

static void register_hooks(apr_pool_t *p)
{
  static const char * const fixupSucc[] = { "mod_dir.c", NULL };

  ap_hook_handler(gridfactory_db_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_fixups(fixup_path, NULL, fixupSucc, APR_HOOK_LAST);
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

