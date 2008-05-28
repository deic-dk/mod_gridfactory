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
 * This program is based on mod_authn_dbd of the Apache foundation,
 * http://svn.apache.org/viewvc/httpd/httpd/trunk/modules/aaa/mod_authn_dbd.c?revision=658046&view=markup.
 * mod_authn_dbd is covered by the pache License, Version 2.0,
 * http://www.apache.org/licenses/LICENSE-2.0
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

/* Keys in the has table of prepared statements. */
static char* LABEL1 = "gridfactory_dbd_1";
static char* LABEL2 = "gridfactory_dbd_2";

/* SQL query to get all job records. */
static char* JOB_REC_SELECT_Q = "SELECT * FROM `jobDefinition`";

/* Prepared statement string to get job record. */
static char* JOB_REC_SELECT_PS = "SELECT * FROM `jobDefinition` WHERE identifier = ?";

/* Prepared statement string to update job record. */
static char* JOB_REC_UPDATE_PS = "UPDATE `jobDefinition` SET status= ?, lastModified = ? WHERE identifier = ?";

/* Optional function - look it up once in post_config. */
static ap_dbd_t *(*dbd_acquire_fn)(request_rec*) = NULL;
static void (*dbd_prepare_fn)(server_rec*, const char*, const char*) = NULL;

/**
 * Configuration
 */
 
typedef struct {
  char* perm_;
} config_rec;

static void*
do_config(apr_pool_t* p, char* d)
{
  config_rec* conf = (config_rec*)apr_pcalloc(p, sizeof(config_rec));
  conf->perm_ = 0;      /* null pointer */
  return conf;
}

static const char*
config_perm(cmd_parms* cmd, void* mconfig, const char* arg)
{
  if (((config_rec*)mconfig)->perm_)
    return "Default permission already set.";

  ((config_rec*)mconfig)->perm_ = (char*) arg;
  
  dbd_prepare(cmd, mconfig);
  
  return 0;
}

/**
 * DB preparation
 */

static void*
dbd_prepare(cmd_parms *cmd, void *cfg)
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
static const command_rec command_table[] =
{
    AP_INIT_TAKE1("DefaultPermission", config_perm,
                  NULL, OR_FILEINFO,
                  "Default permission for directories with no .gacl file. Must be one of none, read or write."),
    {NULL}
};
static authn_status gridsite_db_handler(request_rec *r)
{
    apr_status_t rv;
    const char *job_id = NULL;
    apr_dbd_prepared_t *statement;
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t *row = NULL;
    config_rec* conf;
    
    conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    ap_dbd_t *dbd = dbd_acquire_fn(r);
    if (dbd == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Failed to acquire database connection.");
        return AUTH_GENERAL_ERROR;
    }

    if (conf->perm_ == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                      "No DefaultPermission has been specified");
    }

    statement = apr_hash_get(dbd->prepared, LABEL1, APR_HASH_KEY_STRING);
    if (statement == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "A prepared statement could not be found for getting job records.");
        return AUTH_GENERAL_ERROR;
    }
    if (apr_dbd_pvselect(dbd->driver, r->pool, dbd->handle, &res, statement,
                              0, user, NULL) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Query execution error looking up '%s' "
                      "in database", user);
        return AUTH_GENERAL_ERROR;
    }
    for (rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1);
         rv != -1;
         rv = apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
        if (rv != 0) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                          "Error retrieving results while looking up '%s' "
                          "in database", user);
            return AUTH_GENERAL_ERROR;
        }
        dbd_password = apr_dbd_get_entry(dbd->driver, row, 0);
        /* we can't break out here or row won't get cleaned up */
    }

    if (!dbd_password) {
        return AUTH_USER_NOT_FOUND;
    }

    rv = apr_password_validate(password, dbd_password);

    if (rv != APR_SUCCESS) {
        return AUTH_DENIED;
    }

    return AUTH_GRANTED;
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