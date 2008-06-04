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
static char* JOB_REC_SELECT_PS = "SELECT * FROM `jobDefinition` WHERE identifier = ?";

/* Prepared statement string to update job record. */
static char* JOB_REC_UPDATE_PS = "UPDATE `jobDefinition` SET csStatus= ?, lastModified = ? WHERE identifier = ?";

/* Optional function - look it up once in post_config. */
static ap_dbd_t *(*dbd_acquire_fn)(request_rec*) = NULL;
static void (*dbd_prepare_fn)(server_rec*, const char*, const char*) = NULL;

/* Fields of the jobDefinition table. */
char* fields = "";
/* Number of fields. */
int cols = 0;

/* Forward declaration */
module AP_MODULE_DECLARE_DATA gridfactory_module;


/**
 * Configuration
 */
 
typedef struct {
  char* ps_;
} config_rec;

static void*
do_config(apr_pool_t* p, char* d)
{
  config_rec* conf = (config_rec*)apr_pcalloc(p, sizeof(config_rec));
  conf->ps_ = 0;      /* null pointer */
  return conf;
}

/**
 * DB stuff
 */

static void*
dbd_prepare(cmd_parms* cmd, void* cfg)
{
    static unsigned int label_num = 0;

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

static const command_rec command_table[] =
{
    AP_INIT_TAKE1("PrepareStatements", config_ps,
                  NULL, OR_FILEINFO,
                  "Default permission for directories with no .gacl file. Must be one of none, read or write."),
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
  
    if (strcmp(fields, "") != 0) {
      return 0;
    }
    
    apr_status_t rv;
    const char* ret = "";
    fields = (char *) malloc(1000);
    //fields = apr_pcalloc(r->pool, 1000); --- doesn't seem to work...
      
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t* row = NULL;
    char* val;
    int firstrow = 0;
    
    if (apr_dbd_select(dbd->driver, r->pool, dbd->handle, &res, JOB_REC_SHOW_F_Q, 0) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error");
        return -1;
    }
     
    /* Get the list of fields. */
    ret = apr_pstrcat(r->pool, ret, "dbUrl", NULL);
    ret = apr_pstrcat(r->pool, ret, "\t", NULL);
    for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
         rv != -1;
         rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
        if (rv != 0) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
            return -1;
        }
        val = (char*) apr_dbd_get_entry(dbd->driver, row, 0);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "field --> %s", val);
        if(firstrow != 0){
          ret = apr_pstrcat(r->pool, ret, "\t", NULL);
        }
        ret = apr_pstrcat(r->pool, ret, val, NULL);
        /* we can't break out here or row won't get cleaned up */
        firstrow = -1;
        ++cols;
    }
    
    apr_cpystrn(fields, ret, strlen(ret));
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Found fields:");
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, fields);
    
    //apr_dbd_close(dbd->driver, dbd->handle);
    
    return 0;
}

char* get_job_recs(request_rec *r){
    apr_status_t rv;
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t* row = NULL;
    char* val;
    int i;
    char* recs = "";
    
    ap_dbd_t* dbd = dbd_acquire_fn(r);
    if (dbd == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return NULL;
    }
    
    if (get_fields(r, dbd) < 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to get fields.");
        return NULL;
    }
    
    recs = apr_pstrcat(r->pool, recs, fields, NULL);
       
    if (apr_dbd_select(dbd->driver, r->pool, dbd->handle, &res, JOB_REC_SELECT_Q, 0) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error");
        return NULL;
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
        val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "--> %s", val);
        if(i == 0){
          /* Prepend the pseudo-column 'dbUrl'*/
          // TODO
          recs = apr_pstrcat(r->pool, recs, "URL", NULL);
          recs = apr_pstrcat(r->pool, recs, "\t", NULL);
        }
        else{
          recs = apr_pstrcat(r->pool, recs, "\t", NULL);
        }
        recs = apr_pstrcat(r->pool, recs, val, NULL);
      }
      /* we can't break out here or row won't get cleaned up */
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning rows");
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, recs);
    return recs;
} 

char* get_job_rec(request_rec *r, char* id, char* uuid){
    apr_status_t rv;
    apr_dbd_prepared_t *statement;
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t *row = NULL;
    char* val;
    int i;
    char* rec = 0;
    int firstrow = 0;
    
    ap_dbd_t* dbd = dbd_acquire_fn(r);
    if (dbd == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return NULL;
    }
  
    statement = apr_hash_get(dbd->prepared, LABEL1, APR_HASH_KEY_STRING);
    
    if (statement == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "A prepared statement could not be found for getting job records.");
        return NULL;
    }

    if (dbd == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Failed to acquire database connection.");
        return NULL;
    }
    
    statement = apr_hash_get(dbd->prepared, LABEL1, APR_HASH_KEY_STRING);
    
    if (statement == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "A prepared statement could not be found for getting job records.");
        return NULL;
    }
    
    if (apr_dbd_pvselect(dbd->driver, r->pool, dbd->handle, &res, statement,
                              0, id, NULL) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Query execution error looking up '%s' "
                      "in database", id);
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
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "--> %s", val);
        if(i > 0){
          rec = apr_pstrcat(r->pool, rec, "\t", NULL);
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

char* update_job_rec(request_rec *r, char* id){
} 

static int gridfactory_db_handler(request_rec *r)
{
    char* job_uuid;
    config_rec* conf;
    int uri_len = strlen(r->uri);
    int jobdir_len = strlen(JOB_DIR);
    int ok = 0;
    char* response = "";
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "handler %s", r->handler);

    if (!r->handler || strcmp(r->handler, "gridfactory"))
      return DECLINED;
    
    conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    
    if (conf->ps_ == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PrepareStatements not specified");
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Request: %s", r->the_request);

    /* GET */
    if (r->method_number == M_GET) {
      if(apr_strnatcmp((r->uri) + uri_len - jobdir_len, JOB_DIR) == 0){
        /* GET /grid/db/jobs/?status=ready|requested|running&format=rest|list */
        // TODO: rest|list
        if(r->args && countchr(r->args, "=") > 0){
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "GET %s with args", r->uri);
        }
        /* GET /grid/db/jobs/ */
        else{
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "GET %s with no args", r->uri);
          response = apr_pstrcat(r->pool, response, get_job_recs(r), NULL);
        }
      }    
      /* GET /grid/db/jobs/UUID */
      else if(uri_len > jobdir_len) {
        /* job_uuid = "UUID" */
        job_uuid = memrchr(r->uri, '/', uri_len);
        apr_cpystrn(job_uuid, job_uuid+1 , uri_len - 1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "job_uuid --> %s", job_uuid);
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

