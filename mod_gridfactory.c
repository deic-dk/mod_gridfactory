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
 * three configuration directives are available:
 * 
 *   PrepareStatements "On|Off"
 *      Whether or not MySQL prepared statements should be used. I don't
 *      really see any reason to set this to Off.
 * 
 *   DBBaseURL "URL"
 *      The URL served with this module. If this is not specified,
 *      "https://my.host.hame/db/jobs/" is used.
 * 
 *   XSLDirURL "URL"
 *      Where to find job.xsl, jobs.xsl, history.xsl, nodes.xsl and node.xsl.
 *      These are used for formatting the output when ?mode=xsl is used.
 * 
 * 
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

#define JOB_TABLE_NUM 1
#define HIST_TABLE_NUM 2
#define NODE_TABLE_NUM 3

#define MY_POOL_MAX_FREE_SIZE 128

/* Apache environment variable. This is used to get the DN used for authorizing
 * node updates. */
static const char* CLIENT_S_DN_STRING = "SSL_CLIENT_S_DN";

/* The sub-directory containing the job information. */
static const char* JOB_DIR = "/jobs/";
/* The sub-directory containing the job history. */
static const char* HIST_DIR = "/history/";
/* The sub-directory containing the node information. */
static const char* NODE_DIR = "/nodes/";

/* Keys in the has table of prepared statements. */
static const char* LABEL = "gridfactory_dbd_0";
static const char* LABEL1 = "gridfactory_dbd_1";
static const char* LABEL2 = "gridfactory_dbd_2";
static const char* LABEL3 = "gridfactory_dbd_3";

/* Name of the identifier column. */
static const char* ID_COL = "identifier";

/* Column holding the identifier. */
static int id_col_nr;

/* Name of the status column. */
static const char* STATUS_COL = "csStatus";

/* Name of the name column. */
static const char* NAME_COL = "name";

/* Name of the host column. */
static const char* HOST_COL = "host";

/* Name of the subnodes DB URL column. */
static const char* SUBNODES_DB_URL_COL = "subnodesDbUrl";

/* ready value of the status column. */
static const char* READY = "ready";

/* Name of the lastModified column. */
static const char* LASTMODIFIED_COL = "lastModified";

/* Name of the providerInfo column. */
static const char* PROVIDERINFO_COL = "providerInfo";

/* Name of the providerInfo column. */
static const char* NODEID_COL = "nodeId";

/* Name of the allowedVOs column. */
static const char* ALLOWED_VOS_COL = "allowedVOs";

/* Name of the hypervisors column. */
static const char* HYPERVISORS_COL = "hypervisors";

/* Name of the inputFileURLs column. */
static const char* INPUT_FILE_URLS_COL = "inputFileURLs";

/* Name of the runtimeEnvironments column. */
static const char* RUNTIME_ENVIRONMENTS_COL = "runtimeEnvironments";

/* Name of the outFileMapping column. */
static const char* OUT_FILE_MAPPING_COL = "outFileMapping";

/* Column holding the name. */
static int name_col_nr;

/* Column holding the status. */
static int status_col_nr;

/* Column holding the host. */
static int host_col_nr;

/* Column holding the subnodes DB URL. */
static int subnodes_db_url_col_nr;

/* Name of the DB URL pseudo-column. */
static const char* DBURL_COL = "dbUrl";

/* SQL query to get list of fields. */
static const char* JOB_REC_SHOW_F_Q = "SHOW fields FROM `jobDefinition`";

/* SQL query to get list of fields. */
static const char* HIST_REC_SHOW_F_Q = "SHOW fields FROM `jobHistory`";

/* SQL query to get list of fields. */
static const char* NODE_REC_SHOW_F_Q = "SHOW fields FROM `nodeInformation`";

/* SQL query to get all job definition records. */
static const char* JOB_RECS_SELECT_Q = "SELECT * FROM `jobDefinition`";

/* SQL query to get all job history records. */
static const char* HIST_RECS_SELECT_Q = "SELECT * FROM `jobHistory`";

/* SQL query to get all node information records. */
static const char* NODE_RECS_SELECT_Q = "SELECT * FROM `nodeInformation`";

/* Prepared statement string to get job record. */
static const char* JOB_REC_SELECT_PS = "SELECT * FROM jobDefinition WHERE identifier LIKE ?";

/* Prepared statement string to get history record. */
static const char* HIST_REC_SELECT_PS = "SELECT * FROM jobHistory WHERE identifier LIKE ?";

/* Prepared statement string to get node record. */
static const char* NODE_REC_SELECT_PS = "SELECT * FROM nodeInformation WHERE identifier LIKE ?";

/* Query to get job record. */
static const char* JOB_REC_SELECT_Q = "SELECT * FROM `jobDefinition` WHERE identifier LIKE '%/%s'";

/* Query to get history record. */
static const char* HIST_REC_SELECT_Q = "SELECT * FROM `jobHistory` WHERE identifier LIKE '%/%s'";

/* Query to get node record. */
static const char* NODE_REC_SELECT_Q = "SELECT * FROM `nodeInformation` WHERE identifier = '%s'";

/* Prepared statement string to update job record. */
static const char* JOB_REC_UPDATE_PS_1 = "UPDATE `jobDefinition` SET lastModified = NOW() WHERE identifier LIKE ?";

/* Query to update job record. */
static const char* JOB_REC_UPDATE_Q = "UPDATE `jobDefinition` SET lastModified = NOW()";

/* Query to update node record. */
static const char* NODE_REC_UPDATE_Q = "UPDATE `nodeInformation` SET lastModified = NOW()";

/* Query to create node record. */
static const char* NODE_REC_INSERT_Q = "INSERT INTO nodeInformation SET created = NOW(), lastModified = NOW()";

/* Optional function - look it up once in post_config. */
static ap_dbd_t* (*dbd_acquire_fn)(request_rec*) = NULL;
static void (*dbd_prepare_fn)(server_rec*, const char*, const char*) = NULL;

/* Max size of each field name. */
static int MAX_F_SIZE = 256;

/* Max size of all field names. */
static int MAX_T_F_SIZE = 5120;

/* Max size (bytes) of response and PUT bodies.
 * This is to protect against memory leaking.
 * Notice that it should be MAX_SELECT_ROWS * [longest expected row]*/
static int MAX_SIZE = 1000000;

/* Max number of rows that we will return from a DB query.
 * This is to protect against memory leaking of the parsing functions. */
static int MAX_SELECT_ROWS = 10000;

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

/* Public fields of the jobDefinition table. */
static char* JOB_PUB_FIELDS_STR = "identifier\tname\tcsStatus\tuserInfo\tcreated\tlastModified\trunningSeconds\tramMb\topSys\truntimeEnvironments\tallowedVOs\tvirtualize\tdbUrl";

/* Public fields of the nodeInformation table. */
static char* NODE_PUB_FIELDS_STR = "identifier\thost\tsubNodesDbUrl\tmaxJobs\tallowedVOs\tvirtualize\thypervisors\tmaxMBPerJob\tproviderInfo\tcreated\tlastModified\tdbUrl";

/* Value indicating output should be text formatted. */
static int TEXT_FORMAT = 0;

/* Value indicating output should be XML formatted. */
static int XML_FORMAT = 1;

/* Base URL for the DB web service. */
char* base_url;

/* URL to directory containing job.xsl, jobs.xsl, history.xsl, node.xsl and nodes.xsl. */
char* xsl_dir;

/* Forward declaration */
module AP_MODULE_DECLARE_DATA gridfactory_module;


/**
 * Configuration
 */
 
typedef struct {
  char* ps_;
  char* url_;
  char* xsl_;
} config_rec;

static void*
do_config(apr_pool_t* p, char* d)
{
	/* Apparently ap_log_perror only works for log levels higher than APLOG_INFO,
	   i.e. not with APLOG_INFO and APLOG_DEBUG. */
  //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Doing DB config with %s", p);
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
  conf->xsl_ = 0;      /* null pointer */
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
    dbd_prepare_fn(cmd->server, HIST_REC_SELECT_PS, LABEL2);
    dbd_prepare_fn(cmd->server, NODE_REC_SELECT_PS, LABEL3);
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
  if(((config_rec*)mconfig)->url_){
    return "DBBaseURL already set.";
  }
  ((config_rec*)mconfig)->url_ = (char*) arg;
  return 0;
}

static const char*
config_xsl(cmd_parms* cmd, void* mconfig, const char* arg)
{
  if(((config_rec*)mconfig)->xsl_){
    return "XSLDirURL already set.";
  }
  ((config_rec*)mconfig)->xsl_ = (char*) arg;
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
    AP_INIT_TAKE1("XSLDirURL", config_xsl,
                  NULL, OR_FILEINFO,
                  "Where to get XSL files for formatting XML output."),
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
    /* Used only by update_rec, to check if a job is
     * "ready" before allowing writing. If a job is "ready"
     * only writing the csStatus, nodeId and providerInfo is allowed. */
    char* status;
    /* Used only by update_rec to check if a node record
     * was created by the user trying to modify it. */
    char* providerInfo;
} db_result;

int tokenize_fields_str(apr_pool_t* p, char* fields_str, char** fields, const char* delim){

  char* field;
  char* last;
  int i = 0;
  
  // Use a copy of pub_fields_str (it's modified by the tokenizing)
  char* tmp_fields_str = (char*)apr_pcalloc(p, 512 * sizeof(char*));
  apr_cpystrn(tmp_fields_str, fields_str, strlen(fields_str)+1);
  /* Split the fields on "\t" */
  for(field = apr_strtok(tmp_fields_str, delim, &last); field != NULL;
      field = apr_strtok(NULL, delim, &last)){
    //fields[i] = malloc(MAX_F_SIZE * sizeof(char));
    fields[i] = (char*)apr_pcalloc(p, MAX_F_SIZE * sizeof(char*));
    if(fields[i] == NULL){
      ap_log_perror(APLOG_MARK, APLOG_CRIT, 0, p, "Out of memory.");
      return -1;
    }
    apr_cpystrn(fields[i], field, strlen(field)+1);
    ++i;
  }

  return i;
}

char** set_fields(apr_pool_t* p, ap_dbd_t* dbd, char* fields_str, char* query){
  
    apr_status_t rv;
    const char* ret = "";
    // Hmm, does not work. Memory gets overwritten...
    //fields_str = apr_pcalloc(p, MAX_T_F_SIZE * sizeof(char));
    /*fields_str = (char*) malloc(MAX_T_F_SIZE * sizeof(char*));
    if(fields_str == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Out of memory.");
      return NULL;
    }*/
      
    apr_dbd_results_t *res = NULL;
    apr_dbd_row_t* row;
    char* val;
    int firstrow = 0;
    int i = 0;
    int first_row_num = 1;
    if(APR_VERSION<130){
    	first_row_num=0;
    }
    
    /*ap_dbd_t* dbd = (ap_dbd_t*)apr_pcalloc(p, 256 * sizeof(ap_dbd_t*));
    dbd = dbd_acquire_fn(r);
    if(dbd == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return NULL;
    }*/

    // This crashes with last argument 0 - i.e. only random access works
    int ret_val = apr_dbd_select(dbd->driver, p, dbd->handle, &res, query, 1);
    if(ret_val != 0){
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p, "Query execution error in set_fields, %i.", ret_val);
        return NULL;
    }
    
    int cols = apr_dbd_num_tuples(dbd->driver, res);
    
    //char** fields = malloc(cols * sizeof(char*));
    char** fields = (char**)apr_pcalloc(p, cols * sizeof(char*));
    
    if(fields == NULL){
      ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p,
        "Out of memory while allocating %i columns.", cols);
      return NULL;
    }
    for(i = 0; i < cols; i++){
    //fields[i] = malloc(MAX_F_SIZE * sizeof(char));
    fields[i] = (char*)apr_pcalloc(p, MAX_F_SIZE * sizeof(char*));
      if(fields[i] == NULL){
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p, "Out of memory.");
        return NULL;
      }
    }
    
    /* Get the list of fields. */
    i = 0;
    while(i<cols){
        row = NULL;
        // We have to use last argument 1, ... NOT -1. Only random access works.
        // Older libaprutil1 may start with 0 instead of 1...
        rv = apr_dbd_get_row(dbd->driver, p, res, &row, i+first_row_num);
        if(rv != 0){
            ap_log_perror(APLOG_MARK, APLOG_ERR, rv, p, "Error retrieving results");
            return NULL;
        }
        val = (char*) apr_dbd_get_entry(dbd->driver, row, 0);
        if(firstrow != 0){
          ret = apr_pstrcat(p, ret, "\t", NULL);
        }
        ret = apr_pstrcat(p, ret, val, NULL);
        apr_cpystrn(fields[i], val, strlen(val)+1);
        //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "field --> %s", fields[i]);
        if(apr_strnatcmp(val, ID_COL) == 0){
          id_col_nr = i;
        }
        if(apr_strnatcmp(val, NAME_COL) == 0){
          name_col_nr = i;
        }
        if(apr_strnatcmp(val, STATUS_COL) == 0){
          status_col_nr = i;
        }
        if(apr_strnatcmp(val, HOST_COL) == 0){
          host_col_nr = i;
        }
        if(apr_strnatcmp(val, SUBNODES_DB_URL_COL) == 0){
          subnodes_db_url_col_nr = i;
        }
        firstrow = -1;
        i++;
        /* we can't break out here or row won't get cleaned up */
    }
    /* append the pseudo-column 'dbUrl' */
    ret = apr_pstrcat(p, ret, "\t", NULL);
    ret = apr_pstrcat(p, ret, DBURL_COL, NULL);    
    
    apr_cpystrn(fields_str, ret, strlen(ret)+1);
    
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Found fields: %s; first field: %s", fields_str, fields[0]);
    
    return fields;
}

char* constructUUID(apr_pool_t* p, char* job_id){
  //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Constructing URL from %s", job_id);
  char* uuid = memrchr(job_id, '/', strlen(job_id) - 1);
  if(uuid != NULL){
  	uuid = uuid + 1;
  }
  else{
  	uuid = job_id;
  }
  return uuid;
}

char* recs_text_format(apr_pool_t* p, ap_dbd_t* dbd, apr_dbd_results_t *res,
   int priv, char* pub_fields_str, char* fields_str, char** fields){
  apr_status_t rv;
  char* val;
  apr_dbd_row_t* row;
  int i = 0;
  char* uuid = "";
  char* checkStr;
  char* checkStart;
  char* checkEnd;
  int fieldLen;
  char* recs = malloc(MAX_SIZE);
  
  int cols = apr_dbd_num_cols(dbd->driver,res);
  // Works only for synchronous selects (1 instead of 0)
  //int numrows = apr_dbd_num_tuples(dbd->driver,res);

  if(priv){
    strcpy(recs, pub_fields_str);
  }
  else{
    strcpy(recs, fields_str);
  }
  
  int rownum = 0;
  //while(rownum <= numrows){
  while(rownum<MAX_SELECT_ROWS){
    row = NULL;
    rv = apr_dbd_get_row(dbd->driver, p, res, &row, -1);
    if(rv != 0){
      break;
    }
    strcat(recs, "\n");
    ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "retrieved row %i, cols %i", rownum, cols);
    for(i = 0 ; i < cols ; i++){
      // To check if a field is a member of pub_fields, just see if pub_fields_str contains
      // "\tfield\t".
      checkStr = strstr(pub_fields_str, fields[i]);
      fieldLen = strlen(fields[i]);
      checkStart = checkStr-1;
      checkEnd = checkStr+fieldLen;
      ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Checking field %s",
          fields[i]);
      if(priv && checkStr != pub_fields_str && (
        checkStr == NULL ||
        checkStart != NULL && *checkStart != '\t' ||
        checkEnd != NULL && *checkEnd != '\t'
        )){
        continue;
      }
      val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
      ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "--> %s", val);
      strcat(recs, val);
      strcat(recs, "\t");
      if(i == id_col_nr){
        uuid = constructUUID(p, val);
      }
    }
    strcat(recs, base_url);
    strcat(recs, uuid);
    /* we can't break out here or row won't get cleaned up */
    rownum++;
  }
  if(rownum>=MAX_SELECT_ROWS-1){
    ap_log_perror(APLOG_MARK, APLOG_WARNING, 0, p, "WARNING: max number of rows reached by recs_text_format.");
  }
  
  ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Returning %i rows", rownum);
  ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "%s", recs);

  char* ret = (char*)apr_pcalloc(p, strlen(recs) * sizeof(char*));
  apr_cpystrn(ret, recs, strlen(recs)+1);
  free(recs);
  return ret;
}

char* recs_xml_format(apr_pool_t* p, ap_dbd_t* dbd, apr_dbd_results_t *res,
   int priv, int table_num){
  apr_status_t rv;
  char* val;
  apr_dbd_row_t* row;
  int i = 0;
  char* id = "";
  char* recs = malloc(MAX_SIZE);
  char* rec_name = (char*)apr_pcalloc(p, 8 * sizeof(char*));
  char* list_name = (char*)apr_pcalloc(p, 8 * sizeof(char*));
  
  switch(table_num){
    case JOB_TABLE_NUM:
      sprintf(rec_name, "job");
      sprintf(list_name, "jobs");
      break;
    case HIST_TABLE_NUM:
      sprintf(rec_name, "job");
      sprintf(list_name, "history");
      break;
    case NODE_TABLE_NUM:
      sprintf(rec_name, "node");
      sprintf(list_name, "nodes");
      break;
    default:
      ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p, "Invalid path: %i", table_num);
  }

  strcpy(recs, "<?xml version=\"1.0\"?>\n<?xml-stylesheet type=\"text/xsl\" href=\"");
  strcat(recs, xsl_dir);
  strcat(recs, list_name);
  strcat(recs, ".xsl\"?>\n<");
  strcat(recs, list_name);
  strcat(recs, ">");

  //int numrows = apr_dbd_num_tuples(dbd->driver,res);
  int cols = apr_dbd_num_cols(dbd->driver,res);
  int rownum = 0;
  while(rownum<MAX_SELECT_ROWS){
    row = NULL;
    rv = apr_dbd_get_row(dbd->driver, p, res, &row, -1);
    if (rv != 0) {
      break;
    }
    strcat(recs, "\n  <");
    strcat(recs, rec_name);
    strcat(recs, ">");
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "cols: %i, %i, %i", status_col_nr, host_col_nr, subnodes_db_url_col_nr);
    for (i = 0 ; i < cols ; i++) {
      val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
      //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "%i/%i --> %s", table_num, i, val);
      if(val == NULL){
      	continue;
      }
      if(i == id_col_nr){
        id = val;
        strcat(recs, "\n    <");
        strcat(recs, ID_COL);
        strcat(recs, ">");
        strcat(recs, val);
        strcat(recs, "</");
        strcat(recs, ID_COL);
        strcat(recs, ">");
      }
      else if((table_num == JOB_TABLE_NUM || table_num == HIST_TABLE_NUM)  && i == name_col_nr){
        strcat(recs, "\n    <");
        strcat(recs, NAME_COL);
        strcat(recs, ">");
        strcat(recs, val);
        strcat(recs, "</");
        strcat(recs, NAME_COL);
        strcat(recs, ">");
      }
      else if((table_num == JOB_TABLE_NUM || table_num == HIST_TABLE_NUM)  && i == status_col_nr){
        strcat(recs, "\n    <");
        strcat(recs, STATUS_COL);
        strcat(recs, ">");
        strcat(recs,  val);
        strcat(recs, "</");
        strcat(recs, STATUS_COL);
        strcat(recs, ">");
      }
      else if(table_num == NODE_TABLE_NUM && i == host_col_nr){
        strcat(recs, "\n    <");
        strcat(recs, HOST_COL);
        strcat(recs, ">");
        strcat(recs, val);
        strcat(recs, "</");
        strcat(recs, HOST_COL);
        strcat(recs, ">");
      }
      else if(table_num == NODE_TABLE_NUM && i == subnodes_db_url_col_nr){
        strcat(recs, "\n    <");
        strcat(recs, SUBNODES_DB_URL_COL);
        strcat(recs, ">");
        strcat(recs, val);
        strcat(recs, "</");
        strcat(recs, SUBNODES_DB_URL_COL);
        strcat(recs, ">");
      }
    }
    strcat(recs, "\n    <");
    strcat(recs, DBURL_COL);
    strcat(recs, ">");
    strcat(recs, base_url);
    strcat(recs, constructUUID(p, id));
    strcat(recs, "</");
    strcat(recs, DBURL_COL);
    strcat(recs, ">");
    strcat(recs, "\n  </");
    strcat(recs, rec_name);
    strcat(recs, "> ");
    rownum++;
  }
  if(rownum>=MAX_SELECT_ROWS-1){
   ap_log_perror(APLOG_MARK, APLOG_WARNING, 0, p, "WARNING: max number of rows reached by recs_xml_format.");
  }
  strcat(recs, "\n</");
  strcat(recs, list_name);
  strcat(recs, "> ");
  
  //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Returning rows");
  //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "%s", recs);

  char* ret = (char*)apr_pcalloc(p, strlen(recs) * sizeof(char*));
  apr_cpystrn(ret, recs, strlen(recs)+1);
  free(recs);
  return ret;
}

/**
 * Appends tab separated lines representing DB records to db_result->res, the first line of which
 * is the tab separated list of fields.
 * Setting the switch 'priv' to 1 turns on privacy. Privacy means that only the fields of
 * 'pub_fields' are shown.
 */
db_result* get_recs(request_rec* r, apr_pool_t* p, db_result* ret, int priv, int table_num){
    apr_dbd_results_t *res = NULL;
    char* last;
    char* last1;
    char* token;
    char* subtoken1;
    char* subtoken2;
    int start = -1;
    int end = -1;
    ret->format = 0;
    char* query = (char*)apr_pcalloc(p, 256 * sizeof(char*));
    char* fields_str = (char*)apr_pcalloc(p, 512 * sizeof(char*));
    char** fields;
    char* pub_fields_str = (char*)apr_pcalloc(p, 512 * sizeof(char*));
    char* fields_query = (char*)apr_pcalloc(p, 256 * sizeof(char*));
    char** pub_fields = (char**)apr_pcalloc(p, 256 * sizeof(char**));    
    
    //apr_cpystrn(query, JOB_RECS_SELECT_Q, strlen(JOB_RECS_SELECT_Q)+1);
    switch(table_num){
      case JOB_TABLE_NUM:
        snprintf(query, strlen(JOB_RECS_SELECT_Q)+1, "%s", JOB_RECS_SELECT_Q);
        apr_cpystrn(fields_query, JOB_REC_SHOW_F_Q, strlen(JOB_REC_SHOW_F_Q)+1);
        apr_cpystrn(pub_fields_str, JOB_PUB_FIELDS_STR, strlen(JOB_PUB_FIELDS_STR)+1);
        break;
      case HIST_TABLE_NUM:
        snprintf(query, strlen(HIST_RECS_SELECT_Q)+1, "%s", HIST_RECS_SELECT_Q);
        apr_cpystrn(fields_query, HIST_REC_SHOW_F_Q, strlen(HIST_REC_SHOW_F_Q)+1);
        apr_cpystrn(pub_fields_str, JOB_PUB_FIELDS_STR, strlen(JOB_PUB_FIELDS_STR)+1);
        break;
      case NODE_TABLE_NUM:
        snprintf(query, strlen(NODE_RECS_SELECT_Q)+1, "%s", NODE_RECS_SELECT_Q);
        apr_cpystrn(fields_query, NODE_REC_SHOW_F_Q, strlen(NODE_REC_SHOW_F_Q)+1);
        apr_cpystrn(pub_fields_str, NODE_PUB_FIELDS_STR, strlen(NODE_PUB_FIELDS_STR)+1);
        break;
      default:
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p, "Invalid path: %i --> %s", table_num, r->uri);
    }

    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query0: %s", query);
    
    /* For URLs like
     * GET /db/jobs/?format=text|xml&csStatus=ready|requested|running&...
     * append WHERE statements to query.*/
    if(r->args && countchr(r->args, "=") > 0){
      char buffer[strlen(r->args)+1];
      snprintf(buffer, strlen(r->args)+1, "%s", r->args);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "args: %s", buffer);
      
      for ((token = strtok_r(buffer, "&", &last)); token;
         token = strtok_r(NULL, "&", &last)) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "token: %s", token);
        subtoken1 = strtok_r(token, "=", &last1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken1: %s", subtoken1);
        if(apr_strnatcmp(subtoken1, FORMAT_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          if(apr_strnatcmp(subtoken2, TEXT_FORMAT_STR) == 0){
            ret->format = TEXT_FORMAT;
          }
          else if(apr_strnatcmp(subtoken2, XML_FORMAT_STR) == 0){
            ret->format = XML_FORMAT;
          }
          else{
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Format %s unknown.", subtoken2);
            //return NULL;
          }
        }
        else if(apr_strnatcmp(subtoken1, START_STR) != 0 && apr_strnatcmp(subtoken1, END_STR) != 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken2: %s", subtoken2);
          query = apr_pstrcat(p, query, " WHERE ", subtoken1, " = '", subtoken2, "'", NULL);
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
        query = apr_pstrcat(p, query, " LIMIT ", apr_itoa(p, start), ",", apr_itoa(p, end - start +1), NULL);
      }
      else if(start < 0 && end >= 0){
        query = apr_pstrcat(p, query, " LIMIT ", apr_itoa(p, end +1), NULL);
      }
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query: %s", query);
    }
    /* For a plain URL like GET /db/jobs/, just use query unmodified. */
    else{
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "GET with no args");
    }

   /* If outputting text, set fields. Be ware, after select,
      results MUST be traversed before another select can be done. */
    ap_dbd_t* dbd;// = (ap_dbd_t*)apr_pcalloc(p, sizeof(ap_dbd_t*));
    dbd = dbd_acquire_fn(r);
    if(dbd == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return NULL;
    }
    if((fields=set_fields(p, dbd, fields_str, fields_query))==NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to set fields.");
      return NULL;
    }
    if(ret->format == TEXT_FORMAT){
      if(tokenize_fields_str(p, pub_fields_str, pub_fields, "\t") < 0 && priv){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to set public fields.");
        return NULL;
      }
    }

    /* Now do the query */
    if(apr_dbd_select(dbd->driver, p, dbd->handle, &res, query, 0) != 0){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error in get_recs.");
      return NULL;
    }

    // format result
    if(ret->format == TEXT_FORMAT){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning text");
      ret->res = recs_text_format(p, dbd, res, priv, pub_fields_str, fields_str, fields);
    }
    else if(ret->format == XML_FORMAT){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Returning XML");
      ret->res = recs_xml_format(p, dbd, res, priv, table_num);
    }
    
    // Dont't do this. It causes segfaults...
    //dbd->pool = NULL;
    //apr_dbd_close(dbd->driver, dbd->handle);
    
    return ret;
}

void get_rec_s(apr_pool_t* p, ap_dbd_t* dbd, apr_dbd_results_t* res,
   char* uuid, char* query){
  char* my_query = (char*)apr_pcalloc(p, 256 * sizeof(char*));
  snprintf(my_query, strlen(query)+strlen(uuid)-1, query, uuid);
  //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Query: %s", my_query);
  if(apr_dbd_select(dbd->driver, p, dbd->handle, &res, my_query, 0) != 0){
    ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p, "Query execution error in get_rec_s.");
  }
}

void get_rec_ps(apr_pool_t* p, ap_dbd_t* dbd, apr_dbd_results_t* res,
   char* uuid, int table_num){

    apr_dbd_prepared_t* statement;
    char* str;

    switch(table_num){
      case JOB_TABLE_NUM:
        statement = apr_hash_get(dbd->prepared, LABEL, APR_HASH_KEY_STRING);
        str = apr_pstrcat(p, "%", uuid, NULL);
        break;
      case HIST_TABLE_NUM:
        statement = apr_hash_get(dbd->prepared, LABEL2, APR_HASH_KEY_STRING);
        str = apr_pstrcat(p, "/%", uuid, NULL);
        break;
      case NODE_TABLE_NUM:
        statement = apr_hash_get(dbd->prepared, LABEL3, APR_HASH_KEY_STRING);
        str = apr_pstrcat(p, "/%", uuid, NULL);
        break;
      default:
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p, "Invalid path: %i", table_num);
    }

    if(statement == NULL){
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p,
                      "A prepared statement could not be found for getting job records.");
    }
    
    if(apr_dbd_pvselect(dbd->driver, p, dbd->handle, &res, statement,
                              0, str, NULL) != 0) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p,
                      "Query execution error looking up '%s' in database", uuid);
    }
}

void rec_text_format(apr_pool_t* p, ap_dbd_t* dbd, apr_dbd_results_t* res,
   db_result* ret, char** fields){

    apr_dbd_row_t* row;
    apr_status_t rv;
    char* val;
    int i;
    int firstrow = 0;
    char* rec = "";

    int numrows = apr_dbd_num_tuples(dbd->driver,res);
    int cols = apr_dbd_num_cols(dbd->driver,res);
    int rownum = 0;
    while(rownum<MAX_SELECT_ROWS){
      row = NULL;
      rv = apr_dbd_get_row(dbd->driver, p, res, &row, -1);
      if(rv != 0){
         break;
      }
      if(firstrow != 0){
        rec = apr_pstrcat(p, rec, "\n\n", NULL);
      }
      for(i = 0 ; i < cols ; i++){
        val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
        //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "--> %s", val);
        if(i > 0){
          rec = apr_pstrcat(p, rec, "\n", NULL);
        }
        rec = apr_pstrcat(p, rec, fields[i], ": ", val, NULL);
        // Set res.status
        if(strcmp(fields[i], STATUS_COL) == 0 && val != NULL){
          ret->status = val;
        }
        // Set res.providerInfo
        else if(strcmp(fields[i], PROVIDERINFO_COL) == 0 && val != NULL){
          ret->providerInfo = val;
        }
      }
      firstrow = -1;
      rownum++;
      /* we can't break out here or row won't get cleaned up */
    }
    if(rownum>=MAX_SELECT_ROWS-1){
    	ap_log_perror(APLOG_MARK, APLOG_WARNING, 0, p, "WARNING: max number of rows reached by rec_text_format.");
    }
    
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Returning record:");
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "%s", rec);
    
    ret->res = rec;
}

static int is_list_field(char* field){
	// allowedVOs, hypervisors, inputFileURLs, runtimeEnvironments
	if(strcmp(field, ALLOWED_VOS_COL) == 0){
		return 1;
	}
	else if(strcmp(field, HYPERVISORS_COL) == 0){
		return 1;
	}
	else if(strcmp(field, INPUT_FILE_URLS_COL) == 0){
		return 1;
	}
	else if(strcmp(field, RUNTIME_ENVIRONMENTS_COL) == 0){
		return 1;
	}
	return 0;
}

static int is_out_file_mapping_field(char* field){
	if(strcmp(field, OUT_FILE_MAPPING_COL) == 0){
		return 1;
	}
	return 0;
}

static char* list_xml_format(apr_pool_t* p, char* val, char* sub_field){
  char** fields = (char**)apr_pcalloc(p, 256 * sizeof(char**));
  char* tmp_val = "";
	int cols = tokenize_fields_str(p, val, fields, " ");
	int i;
	for(i = 0 ; i < cols ; i++){
		tmp_val = apr_pstrcat(p, tmp_val, "\n    <", sub_field, ">", fields[i],
		   "</", sub_field, ">", NULL);
	}
	tmp_val = apr_pstrcat(p, tmp_val, "\n  ", NULL);
	return tmp_val;
}

static char* out_file_map_format(apr_pool_t* p, char* val){
  char** fields = (char**)apr_pcalloc(p, 256 * sizeof(char**));
  char* tmp_val = "";
  int i;
	int cols = tokenize_fields_str(p, val, fields, " ");
	for(i = 0 ; i < cols ; i = i + 2){
		tmp_val = apr_pstrcat(p, tmp_val, "\n    <source>", fields[i],
		   "</source>", "<destination>", fields[i+1], "</destination>", NULL);
	}
	tmp_val = apr_pstrcat(p, tmp_val, "\n  ", NULL);
	return tmp_val;
}

void rec_xml_format(apr_pool_t* p, ap_dbd_t* dbd, apr_dbd_results_t* res,
   db_result* ret, char** fields, char* rec_name){

    apr_dbd_row_t* row;
    apr_status_t rv;
    char* val = (char*)apr_pcalloc(p, 256 * sizeof(char*));
    char* sub_field = (char*)apr_pcalloc(p, 256 * sizeof(char*));
    int i;
    int firstrow = 0;
    char* rec = "<?xml version=\"1.0\"?>\n<?xml-stylesheet type=\"text/xsl\" href=\"";
    rec = apr_pstrcat(p, rec, xsl_dir, rec_name, ".xsl\"?>\n<", rec_name, ">", NULL);
    char* tmp_val;
    
    int numrows = apr_dbd_num_tuples(dbd->driver,res);
    int cols = apr_dbd_num_cols(dbd->driver,res);
    int rownum = 0;
    while(rownum<MAX_SELECT_ROWS){
      row = NULL;
      rv = apr_dbd_get_row(dbd->driver, p, res, &row, -1);
      if(rv != 0){
         break;
      }
      if(firstrow != 0){
        rec = apr_pstrcat(p, rec, "\n\n", NULL);
      }
      for(i = 0 ; i < cols ; i++){
        val = (char*) apr_dbd_get_entry(dbd->driver, row, i);
        //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "--> %s", val);
        if(val && strcmp(val, "") != 0){
        	// Format allowedVOs, hypervisors, inputFileURLs, runtimeEnvironments
        	if(is_list_field(fields[i])){
        		apr_cpystrn(sub_field, fields[i], strlen(fields[i]));
            tmp_val = list_xml_format(p, val, sub_field);
        	}
        	// Format outFileMapping
        	else if(is_out_file_mapping_field(fields[i])){
        		tmp_val = out_file_map_format(p, val);
        	}
        	else{
        		tmp_val = val;
        	}
          rec = apr_pstrcat(p, rec, "\n  <", fields[i], ">", tmp_val,
             "</", fields[i], ">", NULL);
        }
        // Set res.status
        if(strcmp(fields[i], STATUS_COL) == 0 && val != NULL){
          ret->status = val;
        }
        // Set res.providerInfo
        else if(strcmp(fields[i], PROVIDERINFO_COL) == 0 && val != NULL){
          ret->providerInfo = val;
        }
      }
      firstrow = -1;
      rownum++;
      /* we can't break out here or row won't get cleaned up */
    }
    if(rownum>=MAX_SELECT_ROWS-1){
    	ap_log_perror(APLOG_MARK, APLOG_WARNING, 0, p, "WARNING: max number of rows reached by rec_xml_format.");
    }
    
    rec = apr_pstrcat(p, rec, "\n</", rec_name, "> ", NULL);
    
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Returning record:");
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "%s", rec);
    
    ret->res = rec;

}

/**
 * Get job definition, job history or node information record - which is determined by 'query'.
 */
void get_rec(apr_pool_t* p, request_rec *r, char* uuid, db_result* ret, int table_num){
  
    char* token;
    char* subtoken1;
    char* subtoken2;
    char* last;
    char* last1;
    ret->format = 0;
    char* query = (char*)apr_pcalloc(p, 256 * sizeof(char*));
    char* fields_str = (char*)apr_pcalloc(p, 256 * sizeof(char*));
    char* fields_query = (char*)apr_pcalloc(p, 256 * sizeof(char*));
    char** fields;
    char* rec_name = (char*)apr_pcalloc(p, 8 * sizeof(char*));
    
    switch(table_num){
      case JOB_TABLE_NUM:
        // Don't use snprintf here - it messes up with non-letters...
        //snprintf(query, strlen(JOB_REC_SELECT_Q)+1, JOB_REC_SELECT_Q);
        apr_cpystrn(query, JOB_REC_SELECT_Q, strlen(JOB_REC_SELECT_Q)+1);
        apr_cpystrn(fields_query, JOB_REC_SHOW_F_Q, strlen(JOB_REC_SHOW_F_Q)+1);
        sprintf(rec_name, "job");
        break;
      case HIST_TABLE_NUM:
        apr_cpystrn(query, HIST_REC_SELECT_Q, strlen(HIST_REC_SELECT_Q)+1);
        apr_cpystrn(fields_query, HIST_REC_SHOW_F_Q, strlen(HIST_REC_SHOW_F_Q)+1);
        sprintf(rec_name, "job");
        break;
      case NODE_TABLE_NUM:
        apr_cpystrn(query, NODE_REC_SELECT_Q, strlen(NODE_REC_SELECT_Q)+1);
        apr_cpystrn(fields_query, NODE_REC_SHOW_F_Q, strlen(NODE_REC_SHOW_F_Q)+1);
        sprintf(rec_name, "node");
        break;
      default:
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Invalid path: %s", r->uri);
    }

    apr_dbd_results_t* dbres = (apr_dbd_results_t*)apr_pcalloc(p, sizeof(apr_dbd_results_t*));  
    //apr_dbd_results_t* res = NULL;
  
    ap_dbd_t* dbd = dbd_acquire_fn(r);
    if(dbd == NULL){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
        return;
    }
    
    if((fields=set_fields(p, dbd, fields_str, fields_query))==NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to get fields.");
      return;
    }
    
    config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    if(conf->ps_ == NULL ||
      apr_strnatcasecmp(conf->ps_, "On") != 0){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PrepareStatements not enabled, %s.", conf->ps_);
      get_rec_s(p, dbd, dbres, uuid, query);
    }
    else{
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PrepareStatements enabled, %s.", conf->ps_);
      get_rec_ps(p, dbd, dbres, uuid, table_num);
    }
    
    if(dbres == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Nothing returned from query.");
      return;
    }
    
    if(r->args && countchr(r->args, "=") > 0){
      char buffer[strlen(r->args)+1];
      snprintf(buffer, strlen(r->args)+1, "%s", r->args);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "args: %s", buffer);
      
      for ((token = strtok_r(buffer, "&", &last)); token;
         token = strtok_r(NULL, "&", &last)) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "token: %s", token);
        subtoken1 = strtok_r(token, "=", &last1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "subtoken1: %s", subtoken1);
        if(apr_strnatcmp(subtoken1, FORMAT_STR) == 0){
          subtoken2 = strtok_r(NULL, "=", &last1);
          if(apr_strnatcmp(subtoken2, TEXT_FORMAT_STR) == 0){
            ret->format = TEXT_FORMAT;
          }
          else if(apr_strnatcmp(subtoken2, XML_FORMAT_STR) == 0){
            ret->format = XML_FORMAT;
          }
          else{
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Format %s unknown.", subtoken2);
            //return NULL;
          }
        }
      }
    }
    if(ret->format == TEXT_FORMAT){
      rec_text_format(p, dbd, dbres, ret, fields);
    }
    else if(ret->format == XML_FORMAT){
      rec_xml_format(p, dbd, dbres, ret, fields, rec_name);
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Returning:");
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "%s",  ret->res);
    
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
static apr_hash_t* parse_put_from_string(apr_pool_t* p, char* args){
  apr_hash_t* tbl;
  char* pair;
  char* eq;
  const char* delim = "\n";
  const char* sep = ":";
  char* last;
  
  if(args == NULL){
    return NULL;
  }
  
  tbl = apr_hash_make(p);
  
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
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "key: %s", pair);
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "value: %s", eq);
    apr_hash_set(tbl, pair, APR_HASH_KEY_STRING, apr_pstrdup(p, eq));
    
  }
  return tbl;
}

/* From The Apache Modules Book. */
static int parse_input_from_put(apr_pool_t* p, request_rec* r, apr_hash_t **form){
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
  
  bb = apr_brigade_create(p, r->connection->bucket_alloc);
  bbin = apr_brigade_create(p, r->connection->bucket_alloc);
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
  buf = apr_palloc(p, count + 1);
  rv = apr_brigade_flatten(bb, buf, &count);
  if(rv != APR_SUCCESS){
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Error (flatten) reading form data.");
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  buf[count] = '\0';
  *form = parse_put_from_string(p, buf);
  
  return OK;
  
}

char* mk_sql_key_values(apr_pool_t* p, apr_hash_t *ht) {
	char* tmp_query = "";
  apr_hash_index_t *hi;
  void *val;
  const void *key;
  //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "Adding key/value pairs to query %s", tmp_query);
  for(hi = apr_hash_first(p, ht); hi; hi = apr_hash_next(hi)){
    apr_hash_this(hi, &key, NULL, &val);
    //ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, p, "%s --> %s", (char *) key, (char *) val);
    tmp_query = apr_pstrcat(p, tmp_query, ", ", key, " = '", val, "'", NULL);
  }
  return tmp_query;
}

int update_rec(apr_pool_t* p, request_rec *r, char* uuid, int table_num) {
  
  /* Read and parse the data. */
  apr_hash_t* put_data = NULL;
  int status = parse_input_from_put(p, r, &put_data);
  if(status != OK){
     ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Error reading request body.");
     return status;
  }
  
  /* Determine the kind of update to be done. */
  apr_hash_index_t* index;
  char* update_query = "";
  char* select_query = "";
  db_result ret = {0, "", ""};
  const char* provider = NULL;
  
  switch(table_num){
    case JOB_TABLE_NUM:
         update_query = apr_pstrcat(p, update_query, JOB_REC_UPDATE_Q, NULL);
         break;
    case NODE_TABLE_NUM:
         update_query = apr_pstrcat(p, update_query, NODE_REC_UPDATE_Q, NULL);
         break;
    default:
         ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Invalid path: %s", r->uri);
         // Modifying history records is not allowed via this web service.
         return DECLINED;
  }

  int nrows;
  /* 0 -> no fields to be updated (except for lastModified),
     1 -> only csStatus, nodeId or providerInfo to be updated,
     2 -> other than csStatus, nodeId or providerInfo to be updated. */
  int status_only = 0;
  for(index = apr_hash_first(NULL, put_data);
     index; index = apr_hash_next(index)){
    const char *k;
    const char *v;
    apr_hash_this(index, (const void**)&k, NULL, (void**)&v);
    if(status_only == 0 && (apr_strnatcmp(k, STATUS_COL) == 0 ||
       apr_strnatcmp(k, NODEID_COL) == 0 ||
       apr_strnatcmp(k, PROVIDERINFO_COL) == 0)){
      status_only = 1;
    }
    if(apr_strnatcmp(k, PROVIDERINFO_COL) == 0 ){
      provider = v;
    }
    if(status_only < 2 && apr_strnatcmp(k, LASTMODIFIED_COL) != 0 &&
       apr_strnatcmp(k, STATUS_COL) != 0 &&
       apr_strnatcmp(k, PROVIDERINFO_COL) != 0 &&
       apr_strnatcmp(k, NODEID_COL) != 0){
      status_only = 2;
    }
    if(apr_strnatcmp(k, LASTMODIFIED_COL) != 0){
      update_query = apr_pstrcat(p, update_query, ", ", ap_escape_html(p, k), " = '",
                        ap_escape_html(p, v), "'", NULL);
    }
  }
  
  /* If someone is trying to update other jobDefintion fields than csStatus or
     changing only lastModified, check if the job status starts with 'ready';
     if it does, decline. */
  if(table_num == JOB_TABLE_NUM && status_only == 2){
    select_query = apr_pstrcat(p, select_query, JOB_REC_SELECT_Q, NULL);
    get_rec(p, r, uuid, &ret, table_num);
    if(&ret != NULL && ret.status && strlen(ret.status)>0 && strstr(READY, ret.status) != NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
         "For %s jobs, only changing csStatus, nodeId and providerInfo is allowed. %i --> %s",
         ret.status, status_only, update_query);
      return DECLINED;
    }
  }
  /* If someone is trying to update a nodeInformation record they did not create, decline. */
  else if(table_num == NODE_TABLE_NUM){
    select_query = apr_pstrcat(p, select_query, NODE_REC_SELECT_Q, NULL);
    get_rec(p, r, uuid, &ret, table_num);
    if(&ret != NULL){
    	// Get the client DN
      request_rec* subreq = ap_sub_req_lookup_file("/dev/null", r, 0);
      const char* client_dn = apr_table_get(subreq->subprocess_env, CLIENT_S_DN_STRING);
      if(ret.providerInfo && strcmp(client_dn, ret.providerInfo) != 0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
           "An existing nodeInformation record can only be changed by its creator %s <-> %s",
           client_dn, ret.providerInfo);
        return DECLINED;
      }
    }
  }

  /* Now update the database record. */
  config_rec* conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
  ap_dbd_t* dbd = dbd_acquire_fn(r);
  if(dbd == NULL){
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Failed to acquire database connection.");
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  
  if(table_num == JOB_TABLE_NUM && conf->ps_ != NULL && apr_strnatcasecmp(conf->ps_, "On") == 0 &&
     status_only == 0){
    /* If no key is present, use prepared statement. */
    apr_dbd_prepared_t* statement = apr_hash_get(dbd->prepared, LABEL1, APR_HASH_KEY_STRING);
    if(statement == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "A prepared statement could not be found for putting job records.");
    }
    char* str = apr_pstrcat(p, "%/", uuid, NULL);
    if(apr_dbd_pvquery(dbd->driver, p, dbd->handle, &nrows,
       statement, str) != 0) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error for %s.", str);
    }
  }
  else{
    /* Otherwise, just use a normal query. */
    // If a nodeInformation record does not exist, create it.
    if(table_num == NODE_TABLE_NUM){
      if(strcmp(ret.res, "") == 0){
        update_query = apr_pstrcat(p, NODE_REC_INSERT_Q,
           mk_sql_key_values(p, put_data), NULL);
      }
      else{
        update_query = apr_pstrcat(p, update_query, " WHERE ", ID_COL, " = '", uuid, "'", NULL);     
      }
    }
    else{
      update_query = apr_pstrcat(p, update_query, " WHERE ", ID_COL, " LIKE '%/", uuid, "'", NULL);     
    }
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Query: %s", update_query);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Provider: %s", provider);
    if(apr_dbd_query(dbd->driver, dbd->handle, &nrows, update_query) != 0){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Query execution error in update_rec");
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

static int request_handler(apr_pool_t *p, request_rec *r, int uri_len, int table_num) {

    int ok = OK;
    char* this_uuid;
    int job_dir_len = strlen(JOB_DIR);
    int hist_dir_len = strlen(HIST_DIR);
    int node_dir_len = strlen(NODE_DIR);
    db_result ret = {0, "", ""};
    //db_result* ret = (db_result*)apr_pcalloc(r->pool, sizeof(db_result*));
    
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "entering request_handler");
    /* GET */
    if(r->method_number == M_GET){
      if(apr_strnatcmp((r->uri) + uri_len - job_dir_len, JOB_DIR) == 0 ||
         apr_strnatcmp((r->uri) + uri_len - hist_dir_len, HIST_DIR) == 0 ||
         apr_strnatcmp((r->uri) + uri_len - node_dir_len, NODE_DIR) == 0){
        /* GET /db/jobs|history|nodes/?... */
        get_recs(r, p, &ret, PRIVATE, table_num);
      }
      /* GET /db/jobs|history|nodes/UUID */
      else if((table_num == JOB_TABLE_NUM && uri_len > job_dir_len) ||
              (table_num == NODE_TABLE_NUM && uri_len > node_dir_len) ||
              (table_num == HIST_TABLE_NUM && uri_len > hist_dir_len)) {
        /* this_uuid = "UUID" */
        /* Chop off any trailing / */
        if (((r->uri)[(strlen(r->uri)-1)]) == '/') {
          (r->uri)[(strlen(r->uri)-1)] = 0;
        }
        this_uuid = memrchr(r->uri, '/', uri_len);
        apr_cpystrn(this_uuid, this_uuid+1 , uri_len - 1);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "this_uuid --> %s", this_uuid);
        get_rec(p, r, this_uuid, &ret, table_num);
      }
      else{
         ok = DECLINED;
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
        ok = DECLINED;
      } 
    }
    /* PUT /db/jobs/UUID */
    /*
     * Test with e.g.
     * curl --insecure --cert /home/fjob/.globus/usercert.pem --key /home/fjob/.globus/userkey.pem \
     * --upload-file 3a86aacc-2d5f-11dd-80f2-c3b981785945 \
     * https://localhost/db/jobs/3a86aacc-2d5f-11dd-80f2-c3b981785945
     */
    else if(r->method_number == M_PUT){
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "PUT %s", r->uri);
      char* tmpstr = (char*)apr_pcalloc(p, sizeof(char*) * 256);
      switch(table_num){
        case JOB_TABLE_NUM:
          apr_cpystrn(tmpstr, strstr(r->uri, JOB_DIR), job_dir_len + 1);
          break;
        case NODE_TABLE_NUM:
          apr_cpystrn(tmpstr, strstr(r->uri, NODE_DIR), node_dir_len + 1);
          break;
        default:
          ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Invalid path: %s", r->uri);
          ok = DECLINED;
      }
      if(!((table_num == JOB_TABLE_NUM && strcmp(JOB_DIR, tmpstr) == 0) ||
         (table_num == HIST_TABLE_NUM && strcmp(HIST_DIR, tmpstr) == 0) ||
         (table_num == NODE_TABLE_NUM && strcmp(NODE_DIR, tmpstr) == 0))) {
        ok = DECLINED;
      }
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Check: %s", tmpstr);
      this_uuid = memrchr(r->uri, '/', uri_len);
      apr_cpystrn(this_uuid, this_uuid+1 , uri_len - 1);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "this_uuid --> %s", this_uuid);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Content type: %s", r->content_type);
      ok = update_rec(p, r, this_uuid, table_num);
    }
    else{
      r->allowed = (apr_int64_t) ((1 < M_GET) | (1 < M_PUT));
    }
    
    return ok;
}

static int gridfactory_db_handler(request_rec *r) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "entering gridfactory_db_handler");

    config_rec* conf;
    int uri_len = 0;
    if (r->uri != NULL) uri_len = strlen(r->uri);
    char* base_path;
    char* tmp_url = "";
    char* path_end;
    // This is either /jobs/ /history/ or /nodes/
    char* main_path;
    // This is set to 1, 2 or 3 in each of the above cases
    int table_num = 0;
    
    // Create a memory pool for this session
    apr_pool_t *p;
    apr_status_t rv = apr_pool_create(&p, r->connection->pool);
    if (rv != APR_SUCCESS) {
      ap_log_rerror(APLOG_MARK, APLOG_CRIT, rv, r, "Failed to create subpool for gridfactory_module");
      return -1;
    }
    //apr_allocator_t* pa = apr_pool_allocator_get(p);
    //apr_allocator_max_free_set(pa, MY_POOL_MAX_FREE_SIZE);
    //apr_pool_cleanup_register(r->connection->pool, p, (const void*) cleanup_pool, apr_pool_cleanup_null);

    if (r->per_dir_config == NULL) {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "directory for mod_gridfactory is null. Maybe your config is missing a Directory directive.");
      return DECLINED;      
    }    
    conf = (config_rec*)ap_get_module_config(r->per_dir_config, &gridfactory_module);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "handler %s", r->handler);

    if (!r->handler || strcmp(r->handler, "gridfactory")){
      return DECLINED;
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "URI: %s", r->uri);

    base_path = (char*)apr_pcalloc(p, sizeof(char*) * 256);
    main_path = (char*)apr_pcalloc(p, sizeof(char*) * 256);
    apr_cpystrn(base_path, r->uri, uri_len + 1);
    path_end = strstr(base_path, JOB_DIR);
    if(path_end != NULL){
      table_num = JOB_TABLE_NUM;
      apr_cpystrn(main_path, JOB_DIR, strlen(JOB_DIR)+1);
      apr_cpystrn(base_path, base_path, uri_len - strlen(path_end) + 1);
      tmp_url = apr_pstrcat(p, tmp_url, base_path, JOB_DIR, NULL);
    }
    if(table_num == 0){
      path_end = strstr(base_path, HIST_DIR);
      if(path_end != NULL){
        table_num = HIST_TABLE_NUM;
        apr_cpystrn(main_path, HIST_DIR, strlen(HIST_DIR)+1);
        apr_cpystrn(base_path, base_path, uri_len - strlen(path_end) + 1);
        tmp_url = apr_pstrcat(p, tmp_url, base_path, HIST_DIR, NULL);
      }
    }
    if(table_num == 0){
      path_end = strstr(base_path, NODE_DIR);
      if(path_end != NULL){
        table_num = NODE_TABLE_NUM;
        apr_cpystrn(main_path, NODE_DIR, strlen(NODE_DIR)+1);
        apr_cpystrn(base_path, base_path , uri_len - strlen(path_end) + 1);
        tmp_url = apr_pstrcat(p, tmp_url, base_path, NODE_DIR, NULL);
      }
    }
    /**
     * If DBBaseURL was not set in the preferences, default to
     * https://this.server/[base]
     * where  [base] is what comes before either /jobs/ /history/ or /nodes/ in
     * request URI. Typically
     * https://this.server/db/
     * With this, base_url will be set to one of
     * https://this.server/db/jobs/, https://this.server/db/history/, https://this.server/db/nodes/ 
     */
    base_url = (char*)apr_pcalloc(p, sizeof(char*) * 256);
    if(conf->url_ == NULL || strcmp(conf->url_, "") == 0){
      tmp_url = apr_pstrcat(p, "https://", r->server->server_hostname, NULL);
      if(r->server->port && r->server->port != 443){
        tmp_url = apr_pstrcat(p, tmp_url, ":", apr_itoa(p, r->server->port), NULL);
      }
      base_url = apr_pstrcat(p, tmp_url, base_path, main_path, NULL);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "DB base URL not set, defaulting base_url to %s, %s, %s",
        base_url, base_path, r->uri);
    }
    else{
      apr_cpystrn(base_url, conf->url_, strlen(conf->url_)+1);
      base_url = apr_pstrcat(p, base_url, main_path, NULL);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "DB base URL set to %s. Setting base_url to %s", conf->url_, base_url);
    }
    
    /**
     * If XSLDirURL was not set in the preferences, default to
     * ../../gridfactory/xsl/.
     */
    xsl_dir = (char*)apr_pcalloc(p, sizeof(char*) * 256);
    if(conf->xsl_ == NULL || strcmp(conf->xsl_, "") == 0){
      tmp_url = "/gridfactory/xsl/";
      apr_cpystrn(xsl_dir, tmp_url, strlen(tmp_url)+1);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "XSL directory URL not set, defaulting xsl_dir to %s, %s",
        xsl_dir, r->uri);
    }
    else{
      apr_cpystrn(xsl_dir, conf->xsl_, strlen(conf->xsl_)+1);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "DB base URL set to %s. Setting xsl_dir to %s", conf->xsl_, xsl_dir);
    }
    
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Request: %s", r->the_request);
    
    // Now delegate to either job_handler, hist_handler or node_handler
    
    int ret = request_handler(p, r, uri_len, table_num);
    
	  ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "cleaning up pool");
	  //ap_dbd_t* dbd = dbd_acquire_fn(r);
	  //apr_pool_destroy(dbd->pool);
    apr_pool_destroy(p);
    //apr_pool_clear(r->connection->pool);

    return ret;
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

