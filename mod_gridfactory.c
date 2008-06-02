/*
 * mod_gridsite
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

module AP_MODULE_DECLARE_DATA authn_dbd_module;

/* The sub-directory containing the jobs. */
static char* JOB_DIR = "/jobs/";

/* The pseudo-file containing the job record. */
static char* JOB_DB_REC = "/db_rec";

/* Keys in the has table of prepared statements. */
static char* LABEL1 = "gridfactory_dbd_1";
static char* LABEL2 = "gridfactory_dbd_2";

/* SQL query to get all job records. */
static char* JOB_REC_SELECT_Q = "SELECT * FROM `jobDefinition`";

/* Prepared statement string to get job record. */
static char* JOB_REC_SELECT_PS = "SELECT * FROM `jobDefinition` WHERE identifier = ?";

/* Prepared statement string to update job record. */
static char* JOB_REC_UPDATE_PS = "UPDATE `jobDefinition` SET csStatus= ?, lastModified = ? WHERE identifier = ?";

/* Optional function - look it up once in post_config. */
static ap_dbd_t *(*dbd_acquire_fn)(request_rec*) = NULL;
static void (*dbd_prepare_fn)(server_rec*, const char*, const char*) = NULL;

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
 * DB preparation
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

static int gridsite_db_handler(request_rec *r)
{
    char* job_uuid;
    config_rec* conf;
    int uri_len = strlen(r->uri);
    int jobdir_len = strlen(JOB_DIR);
    int jobrec_len = strlen(JOB_DB_REC);
    int ok = 0;
    char* response;
    
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
        /* GET /grid/jobs/?status=ready|requested|running */
        if(r->args && countchr(r->args, "=") > 0){
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "GET %s with args", r->uri);
        }
        /* GET /grid/jobs/ */
        else{
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "GET %s with no args", r->uri);
        }
      }    
      /* GET /grid/jobs/UUID/db_rec */
      else if(uri_len > jobdir_len + jobrec_len &&
              apr_strnatcmp((r->uri) + uri_len - jobrec_len, JOB_DB_REC) == 0) {
        /* job_uuid = "/grid/jobs/UUID" */
        apr_cpystrn(job_uuid, r->uri, uri_len - jobrec_len + 1);
        /* job_uuid = "UUID" */
        apr_cpystrn(job_uuid, memrchr(job_uuid, '/', uri_len) +1 , strlen(job_uuid) - jobdir_len);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "job_uuid --> %s", job_uuid);
      }
      else{
        ok = -1;
      }
    }
    /* PUT /grid/db/jobs/UUID/db_rec */
    else if (r->method_number == M_PUT) {
      if(uri_len > jobdir_len + jobrec_len &&
              apr_strnatcmp((r->path_info) + uri_len - jobrec_len, JOB_DB_REC) == 0) {
        apr_cpystrn(job_uuid, memrchr(r->uri, '/', uri_len), uri_len - jobrec_len + 1);
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
      ap_set_content_type(r, "text/html;charset=ascii");
      ap_rputs("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\">\n", r);
      ap_rputs("<html><head><title>Hello World!</title></head>", r);
      ap_rputs("<body><h1>Hello World!</h1></body></html>", r);
    }
    
    return OK;
}

char* get_job_rec(request_rec *r, char* id){
    apr_status_t rv;
    apr_dbd_prepared_t *statement;
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t *row = NULL;
    ap_dbd_t *dbd = dbd_acquire_fn(r);
    char* key;
    char* val;
    int cols;
    int i;
    char* recs = 0;
  
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
} 

char* get_job_recs(request_rec *r){
    apr_status_t rv;
    apr_dbd_prepared_t *statement;
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t *row = NULL;
    ap_dbd_t *dbd = dbd_acquire_fn(r);
    char* key;
    char* val;
    int cols;
    int i;
    char* recs = 0;
    
    if (dbd == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Failed to acquire database connection.");
        return NULL;
    }
    
    
    if (apr_dbd_select(dbd->driver, r->pool, dbd->handle, &res, JOB_REC_SELECT_Q, 0) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Query execution error");
        return NULL;
    }
    
    cols = apr_dbd_num_cols(dbd->driver, res);
    
    /* The first row is the list of colums. */
    for (i = 0 ; i < cols ; i++) {
      //*key = apr_dbd_get_name(dbd->driver, res, i);
      key = (char*) apr_dbd_get_entry(dbd->driver, 0, i);
      recs = apr_pstrcat(r->pool, recs, key, NULL);
      if(i < cols - 1){
        recs = apr_pstrcat(r->pool, recs, "\t", NULL);
      }
    }
        
    for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
         rv != -1;
         rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
        if (rv != 0) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "Error retrieving results");
            return NULL;
        }
        recs = apr_pstrcat(r->pool, recs, '\n', NULL);
        for (i = 0 ; i < cols ; i++) {
          val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
          recs = apr_pstrcat(r->pool, res, val, NULL);
          if(i < cols - 1){
            recs = apr_pstrcat(r->pool, recs, '\t', NULL);
          }
        }
        /* we can't break out here or row won't get cleaned up */
    }
    return recs;
} 

char* update_job_rec(request_rec *r, char* id){
} 

static void register_hooks(apr_pool_t *p)
{
  ap_hook_handler(gridsite_db_handler, NULL, NULL, APR_HOOK_MIDDLE);
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

