#include "sqlite3_drv.h"
#include <stdarg.h>
#include <limits.h>

// MSVC needs "__inline" instead of "inline" in C-source files.
#if defined(_MSC_VER)
#define inline __inline
#endif

#ifdef DEBUG
static int DEBUG = 1;
#else
static int DEBUG = 0;
#endif

#define LOG_DEBUG(M, ...) do { \
    if (DEBUG && drv->log) \
      fprintf(drv->log, "[DEBUG] (%s:%d) " M "\n", __FILE__, __LINE__, __VA_ARGS__); \
  } while (0)
#define LOG_ERROR(M, ...) do { \
    if (drv->log) \
      fprintf(drv->log, "[ERROR] (%s:%d) " M "\n", __FILE__, __LINE__, __VA_ARGS__); \
    if (drv->log != stderr) \
      fprintf(stderr, "[ERROR] (%s:%d) " M "\n", __FILE__, __LINE__, __VA_ARGS__); \
  } while(0)

#define EXTEND_DATASET(n, term_count, term_allocated, dataset) \
  term_count += n; \
  if (term_count > term_allocated) { \
    term_allocated = max(term_count, term_allocated*2); \
    dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated); \
  }

#define EXTEND_DATASET_DIRECT(n) EXTEND_DATASET(n, term_count, term_allocated, dataset)

#define EXTEND_DATASET_PTR(n) EXTEND_DATASET(n, *term_count_p, *term_allocated_p, *dataset_p)

static void append_to_dataset(int n, ErlDrvTermData* dataset, int term_count, ...) {
  int i;
  va_list new_terms;
  va_start(new_terms, term_count);

  for (i = -n; i < 0; i++) {
    dataset[term_count + i] = va_arg(new_terms, ErlDrvTermData);
  }

  va_end(new_terms);
}

static inline ptr_list *add_to_ptr_list(ptr_list *list, void *value_ptr) {
  ptr_list* new_node = driver_alloc(sizeof(ptr_list));
  new_node->head = value_ptr;
  new_node->tail = list;
  return new_node;
}

static inline void free_ptr_list(ptr_list *list, void(* free_head)(void *)) {
  ptr_list* tail;
  while (list) {
    tail = list->tail;
    (*free_head)(list->head);
    driver_free(list);
    list = tail;
  }
}

#ifndef max // macro in Windows
static inline int max(int a, int b) {
  return a >= b ? a : b;
}
#endif

static inline int sql_is_insert(const char *sql) {
  // neither strcasestr nor strnicmp are portable, so have to do this
  int i;
  char *insert = "insert";
  for (i = 0; i < 6; i++) {
    if ((tolower(sql[i]) != insert[i]) && (sql[i] != ' ')) { return 0; }
  }
  return 1;
}

#ifdef DEBUG
static void fprint_dataset(FILE* log, ErlDrvTermData* dataset, int term_count);
#endif

// required because driver_free(_binary) are macros in Windows
static void driver_free_fun(void *ptr) {
  driver_free(ptr);
}

static void driver_free_binary_fun(void *ptr) {
  driver_free_binary((ErlDrvBinary *) ptr);
}

// sdbm from http://www.cse.yorku.ca/~oz/hash.html
unsigned int do_hash(const char *str) {
  unsigned int hash = 0;
  unsigned int c = 0;

  do {
    hash = c + (hash << 6) + (hash << 16) - hash;
    c = *str++;
  } while (c);

  return hash;
}

// Returns a key determined by the file name for an on-disk database,
// determined by the port for a private database.
// This way all access to a single DB will go through one async thread.
static inline unsigned int sql_async_key(char *db_name, ErlDrvPort port) {
  const char *memory_db_name = ":memory:";

  if (strcmp(db_name, memory_db_name)) {
    return do_hash(db_name);
  } else {
    #if ERL_DRV_EXTENDED_MAJOR_VERSION > 2 || \
      (ERL_DRV_EXTENDED_MAJOR_VERSION == 2 && ERL_DRV_EXTENDED_MINOR_VERSION >= 2)
    return driver_async_port_key(port);
    #else
    return (unsigned int) (uintptr_t) port;
    #endif
  }
}

static inline int return_error(
    sqlite3_drv_t *drv, int error_code, const char *error,
    ErlDrvTermData **dataset_p, int *term_count_p, int *term_allocated_p,
    int* error_code_p) {
  if (error_code_p) {
    *error_code_p = error_code;
  }
  EXTEND_DATASET_PTR(9);
  append_to_dataset(9, *dataset_p, *term_count_p,
    ERL_DRV_ATOM, drv->atom_error,
    ERL_DRV_INT, (ErlDrvTermData) error_code,
    ERL_DRV_STRING, (ErlDrvTermData) error, (ErlDrvTermData) strlen(error),
    ERL_DRV_TUPLE, (ErlDrvTermData) 3);
//  int i;
//  for (i = 0; i < *term_count_p; i++) {
//    printf("%d\n", (*dataset_p)[i]);
//  }
  return 0;
}

static inline int output_error(
    sqlite3_drv_t *drv, int error_code, const char *error) {
  int term_count = 2, term_allocated = 13;
  // for some reason breaks if allocated as an array on stack
  // even though it shouldn't be extended
  ErlDrvTermData *dataset = driver_alloc(sizeof(ErlDrvTermData) * term_allocated);
  dataset[0] = ERL_DRV_PORT;
  dataset[1] = driver_mk_port(drv->port);
  return_error(drv, error_code, error, &dataset, &term_count, &term_allocated, NULL);
  term_count += 2;
  dataset[11] = ERL_DRV_TUPLE;
  dataset[12] = 2;
  #ifdef PRE_R16B
  driver_output_term(drv->port,
  #else
  erl_drv_output_term(dataset[1],
  #endif
    dataset, term_count);
  driver_free(dataset);
  return 0;
}

static inline int output_db_error(sqlite3_drv_t *drv) {
  return output_error(drv, sqlite3_errcode(drv->db), sqlite3_errmsg(drv->db));
}

static inline int output_ok(sqlite3_drv_t *drv) {
  // Return {Port, ok}
  ErlDrvTermData spec[] = {
      ERL_DRV_PORT, driver_mk_port(drv->port),
      ERL_DRV_ATOM, drv->atom_ok,
      ERL_DRV_TUPLE, 2
  };
  return
    #ifdef PRE_R16B
    driver_output_term(drv->port,
    #else
    erl_drv_output_term(spec[1],
    #endif
      spec, sizeof(spec) / sizeof(spec[0]));
}

static ErlDrvEntry sqlite3_driver_entry = {
  NULL, /* init */
  start, /* startup (defined below) */
  stop, /* shutdown (defined below) */
  NULL, /* output */
  NULL, /* ready_input */
  NULL, /* ready_output */
  "sqlite3_drv", /* the name of the driver */
  NULL, /* finish */
  NULL, /* handle */
  control, /* control */
  NULL, /* timeout */
  NULL, /* outputv */
  ready_async, /* ready_async (defined below) */
  NULL, /* flush */
  NULL, /* call */
  NULL, /* event */
  ERL_DRV_EXTENDED_MARKER, /* ERL_DRV_EXTENDED_MARKER */
  ERL_DRV_EXTENDED_MAJOR_VERSION, /* ERL_DRV_EXTENDED_MAJOR_VERSION */
  ERL_DRV_EXTENDED_MINOR_VERSION, /* ERL_DRV_EXTENDED_MINOR_VERSION */
  ERL_DRV_FLAG_USE_PORT_LOCKING, /* ERL_DRV_FLAGs */
  NULL /* handle2 */,
  NULL /* process_exit */,
  #if ERL_DRV_EXTENDED_MAJOR_VERSION > 3 || \
  (ERL_DRV_EXTENDED_MAJOR_VERSION == 3 && ERL_DRV_EXTENDED_MINOR_VERSION >= 2)
  NULL /* stop_select */,
  NULL /* emergency_close */
  #else
  NULL /* stop_select */
  #endif
};

DRIVER_INIT(sqlite3_driver) {
  return &sqlite3_driver_entry;
}

// Driver Start
static ErlDrvData start(ErlDrvPort port, char* cmd) {
  sqlite3_drv_t* drv = (sqlite3_drv_t*) driver_alloc(sizeof(sqlite3_drv_t));
  struct sqlite3 *db = NULL;
  int status = 0;
  char *db_name = strstr(cmd, " ");
  size_t db_name_len;
  char *db_name_copy;

#ifdef DEBUG
  errno_t file_open_errno;
  #ifdef _MSC_VER
  const char *log_file = _tempnam(NULL, "erlang-sqlite3-log-");
  file_open_errno = fopen_s(drv->log, log_file, "a+");
  #else
  const char *log_file = tempnam(NULL, "erlang-sqlite3-log-");
  drv->log =
    fopen(log_file, "ax");
  file_open_errno = errno;
  #endif
  if (file_open_errno) {
    fprintf(stderr, "Error creating log file %s; reason %s\n", log_file, strerror(file_open_errno));
    // if we can't open the log file we shouldn't hide the data or the problem
    drv->log = stderr; // noisy
  }
  free(log_file);
#else
  drv->log = NULL;
#endif

#if defined(_MSC_VER)
#pragma warning(disable: 4306)
#endif
  if (!db_name) {
    driver_free(drv);
    return ERL_DRV_ERROR_BADARG;
  } else {
    ++db_name; // move to first character after ' '
  }

  // Create and open the database
  status = sqlite3_open(db_name, &db);
#if defined(_MSC_VER)
#pragma warning(default: 4306)
#endif
  db_name_len = strlen(db_name) + 1; // include terminator
  db_name_copy = driver_alloc(sizeof(char) * db_name_len);
  strcpy(db_name_copy, db_name);

  // Set the state for the driver
  drv->port = port;
  drv->db = db;
  drv->db_name = db_name_copy;
  drv->key = sql_async_key(db_name_copy, port);
  drv->prepared_stmts = NULL;
  drv->prepared_count = 0;
  drv->prepared_alloc = 0;

  drv->atom_blob = driver_mk_atom("blob");
  drv->atom_error = driver_mk_atom("error");
  drv->atom_columns = driver_mk_atom("columns");
  drv->atom_rows = driver_mk_atom("rows");
  drv->atom_null = driver_mk_atom("null");
  drv->atom_rowid = driver_mk_atom("rowid");
  drv->atom_ok = driver_mk_atom("ok");
  drv->atom_done = driver_mk_atom("done");
  drv->atom_unknown_cmd = driver_mk_atom("unknown_command");

  if (status != SQLITE_OK) {
    LOG_ERROR("Unable to open file %s: \"%s\"\n\n", db_name, sqlite3_errmsg(db));
    output_db_error(drv);
  } else {
    LOG_DEBUG("Opened file %s\n", db_name);
    output_ok(drv);
  }

  return (ErlDrvData) drv;
}

// Driver Stop
static void stop(ErlDrvData handle) {
  sqlite3_drv_t* drv = (sqlite3_drv_t*) handle;
  unsigned int i;
  int close_result;

  if (drv->prepared_stmts) {
    for (i = 0; i < drv->prepared_count; i++) {
      sqlite3_finalize(drv->prepared_stmts[i]);
    }
    driver_free(drv->prepared_stmts);
  }

  close_result = sqlite3_close(drv->db);
  if (close_result != SQLITE_OK) {
    LOG_ERROR("Failed to close DB %s, some resources aren't finalized!", drv->db_name);
  }

  if (drv->log && (drv->log != stderr)) {
    fclose(drv->log);
  }

  driver_free(drv->db_name);
  driver_free(drv);
}

// Handle input from Erlang VM
static ErlDrvSSizeT control(
    ErlDrvData drv_data, unsigned int command, char *buf,
    ErlDrvSizeT len, char **rbuf, ErlDrvSizeT rlen) {
  sqlite3_drv_t* drv = (sqlite3_drv_t*) drv_data;
  if (len > INT_MAX) {
    output_error(drv, SQLITE_MISUSE, "Command size doesn't fit into int type");
  } else {
    switch (command) {
    case CMD_SQL_EXEC:
      sql_exec(drv, buf, (int) len);
      break;
    case CMD_SQL_BIND_AND_EXEC:
      sql_bind_and_exec(drv, buf, (int) len);
      break;
    case CMD_PREPARE:
      prepare(drv, buf, (int) len);
      break;
    case CMD_PREPARED_BIND:
      prepared_bind(drv, buf, (int) len);
      break;
    case CMD_PREPARED_STEP:
      prepared_step(drv, buf, (int) len);
      break;
    case CMD_PREPARED_RESET:
      prepared_reset(drv, buf, (int) len);
      break;
    case CMD_PREPARED_CLEAR_BINDINGS:
      prepared_clear_bindings(drv, buf, (int) len);
      break;
    case CMD_PREPARED_FINALIZE:
      prepared_finalize(drv, buf, (int) len);
      break;
    case CMD_PREPARED_COLUMNS:
      prepared_columns(drv, buf, (int) len);
      break;
    case CMD_SQL_EXEC_SCRIPT:
      sql_exec_script(drv, buf, (int) len);
      break;
    case CMD_ENABLE_LOAD_EXTENSION:
      enable_load_extension(drv, buf, (int) len);
      break;
    case CMD_CHANGES:
      changes(drv, buf, (int) len);
      break;
    default:
      unknown(drv, buf, (int) len);
    }
  }
  return 0;
}

static int changes(sqlite3_drv_t *drv, char *buf, int len) {
    int changes = sqlite3_changes(drv->db);
    ErlDrvTermData spec[6];

    spec[0] = ERL_DRV_PORT;
    spec[1] = driver_mk_port(drv->port);
    spec[2] = ERL_DRV_UINT;
    spec[3] = changes;
    spec[4] = ERL_DRV_TUPLE;
    spec[5] = 2;

    return
    #ifdef PRE_R16B
    driver_output_term(drv->port,
    #else
    erl_drv_output_term(spec[1],
    #endif
    spec, sizeof(spec) / sizeof(spec[0]));
}

static int enable_load_extension(sqlite3_drv_t* drv, char *buf, int len) {
#ifdef ERLANG_SQLITE3_LOAD_EXTENSION
  char enable = buf[0];
  int result = sqlite3_enable_load_extension(drv->db, (int) enable);
  if (result) {
    output_db_error(drv);
    return result;
  } else {
    output_ok(drv);
    return 0;
  }
#else
  output_error(drv, SQLITE_MISUSE, "extension loading not enabled, recompile erlang-sqlite3 with ERLANG_SQLITE3_LOAD_EXTENSION defined");
  return -1;
#endif
}

static inline async_sqlite3_command *make_async_command_statement(
    sqlite3_drv_t *drv, sqlite3_stmt *statement, int finalize) {
  async_sqlite3_command *result =
    (async_sqlite3_command *) driver_alloc(sizeof(async_sqlite3_command));
  memset(result, 0, sizeof(async_sqlite3_command));

  result->driver_data = drv;
  result->type = t_stmt;
  result->statement = statement;
  result->finalize_statement_on_free = finalize;
  return result;
}

static inline async_sqlite3_command *make_async_command_script(
    sqlite3_drv_t *drv, char *script, int script_length) {
  async_sqlite3_command *result =
    (async_sqlite3_command *) driver_alloc(sizeof(async_sqlite3_command));
  char *script_copy = driver_alloc(sizeof(char) * script_length);
  memset(result, 0, sizeof(async_sqlite3_command));
  memcpy(script_copy, script, sizeof(char) * script_length);

  result->driver_data = drv;
  result->type = t_script;
  result->script = script_copy;
  result->end = script_copy + script_length;
  return result;
}

static inline void exec_async_command(
    sqlite3_drv_t *drv, void (*async_invoke)(void*),
    async_sqlite3_command *async_command) {
  // Check is required because we are sometimes accessing
  // sqlite3 from the emulator thread. Could also be fixed
  // by making _all_ access except start/stop go through driver_async
  if (sqlite3_threadsafe()) {
    long status = driver_async(drv->port, &drv->key, async_invoke,
                               async_command, sql_free_async);

    // see https://groups.google.com/d/msg/erlang-programming/XiFR6xxhGos/B6ARBIlvpMUJ
    if (status < 0) {
      LOG_ERROR("driver_async call failed: %ld", status);
      output_error(drv, SQLITE_ERROR, "driver_async call failed");
    }
  } else {
    async_invoke(async_command);
    ready_async((ErlDrvData) drv, (ErlDrvThreadData) async_command);
  }
}

static inline int sql_exec_statement(
    sqlite3_drv_t *drv, sqlite3_stmt *statement) {
  async_sqlite3_command *async_command = make_async_command_statement(drv, statement, 1);

  LOG_DEBUG("Driver async: %d %p\n", SQLITE_VERSION_NUMBER, async_command->statement);

  exec_async_command(drv, sql_exec_async, async_command);
  return 0;
}

static int sql_exec(sqlite3_drv_t *drv, char *command, int command_size) {
  int result;
  const char *rest;
  sqlite3_stmt *statement;

  LOG_DEBUG("Preexec: %.*s\n", command_size, command);
  result = sqlite3_prepare_v2(drv->db, command, command_size, &statement, &rest);
  if (result != SQLITE_OK) {
    return output_db_error(drv);
  } else if (statement == NULL) {
    return output_error(drv, SQLITE_MISUSE, "empty statement");
  }
  return sql_exec_statement(drv, statement);
}

static int sql_exec_script(sqlite3_drv_t *drv, char *command, int command_size) {
  async_sqlite3_command *async_command = make_async_command_script(drv, command, command_size);

  LOG_DEBUG("Driver async: %d %p\n", SQLITE_VERSION_NUMBER, async_command->statement);

  exec_async_command(drv, sql_exec_async, async_command);
  return 0;
}

static inline int decode_and_bind_param(
    sqlite3_drv_t *drv, char *buffer, int *p_index,
    sqlite3_stmt *statement, int param_index, int *p_type, int *p_size) {
  int result;
  sqlite3_int64 int64_val;
  double double_val;
  char* char_buf_val;
  long bin_size;

  ei_get_type(buffer, p_index, p_type, p_size);
  switch (*p_type) {
  case ERL_SMALL_INTEGER_EXT:
  case ERL_INTEGER_EXT:
  case ERL_SMALL_BIG_EXT:
  case ERL_LARGE_BIG_EXT:
    ei_decode_longlong(buffer, p_index, &int64_val);
    result = sqlite3_bind_int64(statement, param_index, int64_val);
    break;
  case ERL_FLOAT_EXT:
#ifdef NEW_FLOAT_EXT
  case NEW_FLOAT_EXT: // what's the difference?
#endif
    ei_decode_double(buffer, p_index, &double_val);
    result = sqlite3_bind_double(statement, param_index, double_val);
    break;
  case ERL_ATOM_EXT:
    // include space for null separator
    char_buf_val = driver_alloc((*p_size + 1) * sizeof(char));
    ei_decode_atom(buffer, p_index, char_buf_val);
    if (strncmp(char_buf_val, "null", 5) == 0) {
      result = sqlite3_bind_null(statement, param_index);
    } else {
      output_error(drv, SQLITE_MISUSE, "Non-null atom as parameter");
      return 1;
    }
    break;
  case ERL_STRING_EXT:
    // include space for null separator
    char_buf_val = driver_alloc((*p_size + 1) * sizeof(char));
    ei_decode_string(buffer, p_index, char_buf_val);
    result = sqlite3_bind_text(statement, param_index, char_buf_val, *p_size, &driver_free_fun);
    break;
  case ERL_BINARY_EXT:
    char_buf_val = driver_alloc(*p_size * sizeof(char));
    ei_decode_binary(buffer, p_index, char_buf_val, &bin_size);
    result = sqlite3_bind_text(statement, param_index, char_buf_val, *p_size, &driver_free_fun);
    break;
  case ERL_SMALL_TUPLE_EXT:
    // assume this is {blob, Blob}
    ei_get_type(buffer, p_index, p_type, p_size);
    ei_decode_tuple_header(buffer, p_index, p_size);
    if (*p_size != 2) {
      output_error(drv, SQLITE_MISUSE, "bad parameter type");
      return 1;
    }
    ei_skip_term(buffer, p_index); // skipped the atom 'blob'
    ei_get_type(buffer, p_index, p_type, p_size);
    if (*p_type != ERL_BINARY_EXT) {
      output_error(drv, SQLITE_MISUSE, "bad parameter type");
      return 1;
    }
    char_buf_val = driver_alloc(*p_size * sizeof(char));
    ei_decode_binary(buffer, p_index, char_buf_val, &bin_size);
    result = sqlite3_bind_blob(statement, param_index, char_buf_val, *p_size, &driver_free_fun);
    break;
  default:
    output_error(drv, SQLITE_MISUSE, "bad parameter type");
    return 1;
  }
  if (result != SQLITE_OK) {
    output_db_error(drv);
    return result;
  }
  return SQLITE_OK;
}

static int bind_parameters(
    sqlite3_drv_t *drv, char *buffer, int buffer_size, int *p_index,
    sqlite3_stmt *statement, int *p_type, int *p_size) {
  // decoding parameters
  int i, cur_list_size = -1, param_index = 1, param_indices_are_explicit = 0, result = 0;
  long param_index_long;
  char param_name[MAXATOMLEN + 1]; // parameter names shouldn't be longer than 256!
  char *acc_string;

  result = ei_decode_list_header(buffer, p_index, &cur_list_size);
  if (result) {
    // probably all parameters are integers between 0 and 255
    // and the list was encoded as string (see ei documentation)
    ei_get_type(buffer, p_index, p_type, p_size);
    if (*p_type != ERL_STRING_EXT) {
      return output_error(drv, SQLITE_ERROR,
                          "error while binding parameters");
    }
    acc_string = driver_alloc(sizeof(char) * (*p_size + 1));
    ei_decode_string(buffer, p_index, acc_string);
    for (param_index = 1; param_index <= *p_size; param_index++) {
      sqlite3_bind_int(statement, param_index, (int) (unsigned char) acc_string[param_index - 1]);
    }
    driver_free(acc_string);
    return 0;
  }

  for (i = 0; i < cur_list_size; i++) {
    if (*p_index >= buffer_size) {
      return output_error(drv, SQLITE_ERROR,
                          "error while binding parameters");
    }

    ei_get_type(buffer, p_index, p_type, p_size);
    if (*p_type == ERL_SMALL_TUPLE_EXT) {
      int old_index = *p_index;
      // param with name or explicit index
      param_indices_are_explicit = 1;
      if (*p_size != 2) {
        return output_error(drv, SQLITE_MISUSE,
                            "tuple should contain index or name, and value");
      }
      ei_decode_tuple_header(buffer, p_index, p_size);
      ei_get_type(buffer, p_index, p_type, p_size);
      // first element of tuple is int (index), atom, or string (name)
      switch (*p_type) {
      case ERL_SMALL_INTEGER_EXT:
      case ERL_INTEGER_EXT:
        ei_decode_long(buffer, p_index, &param_index_long);
        param_index = param_index_long;
        break;
      case ERL_ATOM_EXT:
        ei_decode_atom(buffer, p_index, param_name);
        // insert zero terminator
        param_name[*p_size] = '\0';
        if (strncmp(param_name, "blob", 5) == 0) {
          // this isn't really a parameter name!
          *p_index = old_index;
          param_indices_are_explicit = 0;
          goto IMPLICIT_INDEX; // yuck
        } else {
          param_index = sqlite3_bind_parameter_index(statement, param_name);
        }
        break;
      case ERL_STRING_EXT:
        if (*p_size >= MAXATOMLEN) {
          return output_error(drv, SQLITE_TOOBIG, "parameter name too long");
        }
        ei_decode_string(buffer, p_index, param_name);
        // insert zero terminator
        param_name[*p_size] = '\0';
        param_index = sqlite3_bind_parameter_index(statement, param_name);
        break;
      default:
        return output_error(drv, SQLITE_MISMATCH,
                            "parameter index must be given as integer, atom, or string");
      }
      result = decode_and_bind_param(
        drv, buffer, p_index, statement, param_index, p_type, p_size);
      if (result != SQLITE_OK) {
        return result; // error has already been output
      }
    }
    else {
      IMPLICIT_INDEX:
      if (param_indices_are_explicit) {
        return output_error(drv, SQLITE_MISUSE,
                            "parameters without indices shouldn't follow indexed or named parameters");
      }

      result = decode_and_bind_param(
        drv, buffer, p_index, statement, param_index, p_type, p_size);
      if (result != SQLITE_OK) {
        return result; // error has already been output
      }
      ++param_index;
    }
  }
  return result;
}

static void get_columns(
    sqlite3_drv_t *drv, sqlite3_stmt *statement, int column_count, int base,
    int *term_count_p, int *term_allocated_p, ptr_list** p_ptrs, ErlDrvTermData **dataset_p) {
  int i;

  EXTEND_DATASET_PTR(column_count * 3 + 3);
  for (i = 0; i < column_count; i++) {
    const char *column_name = sqlite3_column_name(statement, i);
    size_t column_name_length = strlen(column_name);
    char *column_name_copy = driver_alloc(sizeof(char) * (column_name_length + 1));
    strcpy(column_name_copy, column_name);
    *p_ptrs = add_to_ptr_list(*p_ptrs, column_name_copy);
    LOG_DEBUG("Column: %s\n", column_name_copy);

    (*dataset_p)[base + (i * 3)] = ERL_DRV_STRING;
    (*dataset_p)[base + (i * 3) + 1] = (ErlDrvTermData) column_name_copy;
    (*dataset_p)[base + (i * 3) + 2] = column_name_length;
  }
  (*dataset_p)[base + column_count * 3 + 0] = ERL_DRV_NIL;
  (*dataset_p)[base + column_count * 3 + 1] = ERL_DRV_LIST;
  (*dataset_p)[base + column_count * 3 + 2] = column_count + 1;
}

static int sql_bind_and_exec(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  int result;
  int index = 0;
  int type, size;
  const char *rest;
  sqlite3_stmt *statement;
  long bin_size;
  char *command;

  LOG_DEBUG("Preexec: %.*s\n", buffer_size, buffer);

  ei_decode_version(buffer, &index, NULL);
  result = ei_decode_tuple_header(buffer, &index, &size);
  if (result || (size != 2)) {
    return output_error(drv, SQLITE_MISUSE,
                        "Expected a tuple of SQL command and params");
  }

  // decode SQL statement
  ei_get_type(buffer, &index, &type, &size);
  // TODO support any iolists
  if (type != ERL_BINARY_EXT) {
    return output_error(drv, SQLITE_MISUSE,
                        "SQL should be sent as an Erlang binary");
  }

  command = driver_alloc(size * sizeof(char));
  ei_decode_binary(buffer, &index, command, &bin_size);
  // assert(bin_size == size)
  result = sqlite3_prepare_v2(drv->db, command, size, &statement, &rest);
  driver_free(command);

  if (result != SQLITE_OK) {
    return output_db_error(drv);
  } else if (statement == NULL) {
    return output_error(drv, SQLITE_MISUSE, "empty statement");
  }

  result = bind_parameters(drv, buffer, buffer_size, &index, statement, &type, &size);
  if (result == SQLITE_OK) {
    return sql_exec_statement(drv, statement);
  } else {
    sqlite3_finalize(statement);
    return result; // error has already been output
  }
}

static void sql_free_async(void *_async_command) {
  async_sqlite3_command *async_command =
    (async_sqlite3_command *) _async_command;
  driver_free(async_command->dataset);

  free_ptr_list(async_command->ptrs, &driver_free_fun);

  free_ptr_list(async_command->binaries, &driver_free_binary_fun);

  if ((async_command->type == t_stmt) &&
      async_command->finalize_statement_on_free &&
      async_command->statement) {
    sqlite3_finalize(async_command->statement);
    async_command->statement = NULL;
  } else if (async_command->type == t_script) {
    driver_free(async_command->script);
  }
  driver_free(async_command);
}

static int sql_exec_one_statement(
    sqlite3_stmt *statement, async_sqlite3_command *async_command,
    int *term_count_p, int *term_allocated_p, ErlDrvTermData **dataset_p) {
  int column_count = sqlite3_column_count(statement);
  int row_count = 0, next_row;
  int base_term_count;
  int has_error = 0; // bool
  sqlite3_drv_t *drv = async_command->driver_data;
  ptr_list **ptrs_p = &(async_command->ptrs);
  ptr_list **binaries_p = &(async_command->binaries);
  // printf("\nsql_exec_one_statement. SQL:\n%s\n Term count: %d, terms alloc: %d\n", sqlite3_sql(statement), *term_count_p, *term_allocated_p);

  int i;

  if (column_count > 0) {
    EXTEND_DATASET_PTR(2);
    append_to_dataset(2, *dataset_p, *term_count_p, ERL_DRV_ATOM, drv->atom_columns);
    base_term_count = *term_count_p;
    get_columns(
      drv, statement, column_count, base_term_count, term_count_p, term_allocated_p, ptrs_p, dataset_p);
    EXTEND_DATASET_PTR(4);
    append_to_dataset(4, *dataset_p, base_term_count + column_count * 3 + 7,
      ERL_DRV_TUPLE, (ErlDrvTermData) 2, ERL_DRV_ATOM, drv->atom_rows);
  }

  LOG_DEBUG("Exec: %s\n", sqlite3_sql(statement));

  while ((next_row = sqlite3_step(statement)) == SQLITE_ROW) {
    for (i = 0; i < column_count; i++) {
      LOG_DEBUG("Column %d type: %d\n", i, sqlite3_column_type(statement, i));
      switch (sqlite3_column_type(statement, i)) {
      case SQLITE_INTEGER: {
        ErlDrvSInt64 *int64_ptr = driver_alloc(sizeof(ErlDrvSInt64));
        *int64_ptr = (ErlDrvSInt64) sqlite3_column_int64(statement, i);
        *ptrs_p = add_to_ptr_list(*ptrs_p, int64_ptr);

        EXTEND_DATASET_PTR(2);
        append_to_dataset(2, *dataset_p, *term_count_p, ERL_DRV_INT64, (ErlDrvTermData) int64_ptr);
        break;
      }
      case SQLITE_FLOAT: {
        double *float_ptr = driver_alloc(sizeof(double));
        *float_ptr = sqlite3_column_double(statement, i);
        *ptrs_p = add_to_ptr_list(*ptrs_p, float_ptr);

        EXTEND_DATASET_PTR(2);
        append_to_dataset(2, *dataset_p, *term_count_p, ERL_DRV_FLOAT, (ErlDrvTermData) float_ptr);
        break;
      }
      case SQLITE_BLOB: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        *binaries_p = add_to_ptr_list(*binaries_p, binary);

        EXTEND_DATASET_PTR(8);
        append_to_dataset(8, *dataset_p, *term_count_p,
          ERL_DRV_ATOM, drv->atom_blob,
          ERL_DRV_BINARY, (ErlDrvTermData) binary, (ErlDrvTermData) bytes, (ErlDrvTermData) 0,
          ERL_DRV_TUPLE, (ErlDrvTermData) 2);
        break;
      }
      case SQLITE_TEXT: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        *binaries_p = add_to_ptr_list(*binaries_p, binary);

        EXTEND_DATASET_PTR(4);
        append_to_dataset(4, *dataset_p, *term_count_p,
          ERL_DRV_BINARY, (ErlDrvTermData) binary, (ErlDrvTermData) bytes, (ErlDrvTermData) 0);
        break;
      }
      case SQLITE_NULL: {
        EXTEND_DATASET_PTR(2);
        append_to_dataset(2, *dataset_p, *term_count_p, ERL_DRV_ATOM, drv->atom_null);
        break;
      }
      }
    }
    EXTEND_DATASET_PTR(2);
    append_to_dataset(2, *dataset_p, *term_count_p, ERL_DRV_TUPLE, (ErlDrvTermData) column_count);

    row_count++;
  }

  if (next_row != SQLITE_DONE) {
    if (column_count == 0) {
        return_error(drv, next_row, sqlite3_errmsg(drv->db),
                     dataset_p, term_count_p,
                     term_allocated_p, &async_command->error_code);
        async_command->finalize_statement_on_free = 1;
        return 1;
    } else {
        has_error = 1;
    }
  }

  if (column_count > 0) {
    EXTEND_DATASET_PTR(5);
    append_to_dataset(5, *dataset_p, *term_count_p,
      ERL_DRV_NIL, ERL_DRV_LIST, (ErlDrvTermData) (row_count + 1),
      ERL_DRV_TUPLE, (ErlDrvTermData) 2);

    if (has_error) {
      return_error(drv, next_row, sqlite3_errmsg(drv->db),
                   dataset_p, term_count_p,
                   term_allocated_p, &async_command->error_code);
    }

    EXTEND_DATASET_PTR(3);
    append_to_dataset(3, *dataset_p, *term_count_p,
      ERL_DRV_NIL, ERL_DRV_LIST, (ErlDrvTermData) (3 + has_error));
  } else if (sql_is_insert(sqlite3_sql(statement))) {
    ErlDrvSInt64 *rowid_ptr = driver_alloc(sizeof(ErlDrvSInt64));
    *rowid_ptr = (ErlDrvSInt64) sqlite3_last_insert_rowid(drv->db);
    *ptrs_p = add_to_ptr_list(*ptrs_p, rowid_ptr);
    EXTEND_DATASET_PTR(6);
    append_to_dataset(6, *dataset_p, *term_count_p,
      ERL_DRV_ATOM, drv->atom_rowid,
      ERL_DRV_INT64, (ErlDrvTermData) rowid_ptr,
      ERL_DRV_TUPLE, (ErlDrvTermData) 2);
  } else {
    EXTEND_DATASET_PTR(2);
    append_to_dataset(2, *dataset_p, *term_count_p, ERL_DRV_ATOM, drv->atom_ok);
  }

  LOG_DEBUG("Total term count: %p %d, rows count: %dx%d\n",
    statement, *term_count_p, column_count, row_count);
  async_command->finalize_statement_on_free = 1;

  return has_error;
}

static void sql_exec_async(void *_async_command) {
  async_sqlite3_command *async_command = (async_sqlite3_command *) _async_command;

  sqlite3_stmt *statement = NULL;
  int result;
  const char *rest;
  const char *end;
  int num_statements = 0;
  int term_count = 0, term_allocated = 0;
  ErlDrvTermData *dataset = NULL;

  sqlite3_drv_t *drv = async_command->driver_data;

  EXTEND_DATASET_DIRECT(2);
  append_to_dataset(2, dataset, term_count, ERL_DRV_PORT, driver_mk_port(drv->port));

  switch (async_command->type) {
  case t_stmt:
    statement = async_command->statement;
    sql_exec_one_statement(statement, async_command, &term_count,
                           &term_allocated, &dataset);
    break;
  case t_script:
    rest = async_command->script;
    end = async_command->end;

    while ((rest < end) && !(async_command->error_code)) {
      result = sqlite3_prepare_v2(drv->db, rest, (int) (end - rest), &statement, &rest);
      if (result != SQLITE_OK) {
        // sqlite doc says statement will be NULL here, so no need to finalize it
        num_statements++;
        return_error(drv, result, sqlite3_errmsg(drv->db), &dataset,
                     &term_count, &term_allocated, &async_command->error_code);
        break;
      } else if (statement == NULL) {
        // the script has completed
        break;
      } else {
        num_statements++;
        result = sql_exec_one_statement(statement, async_command, &term_count,
                                        &term_allocated, &dataset);
        sqlite3_finalize(statement);
        if (result) {
          break;
        }
      }
    }

    EXTEND_DATASET_DIRECT(3);
    append_to_dataset(3, dataset, term_count,
      ERL_DRV_NIL, ERL_DRV_LIST, (ErlDrvTermData) (num_statements + 1));
  }

  EXTEND_DATASET_DIRECT(2);
  append_to_dataset(2, dataset, term_count, ERL_DRV_TUPLE, (ErlDrvTermData) 2);

  // print_dataset(dataset, term_count);

  async_command->term_count = term_count;
  async_command->term_allocated = term_allocated;
  async_command->dataset = dataset;
}

static void sql_step_async(void *_async_command) {
  async_sqlite3_command *async_command = (async_sqlite3_command *) _async_command;
  int term_count = 0;
  int term_allocated = 0;
  ErlDrvTermData *dataset = NULL;
  sqlite3_drv_t *drv = async_command->driver_data;

  int column_count = 0;
  sqlite3_stmt *statement = async_command->statement;

  ptr_list *ptrs = NULL;
  ptr_list *binaries = NULL;
  int i;
  int result;

  switch(result = sqlite3_step(statement)) {
  case SQLITE_ROW:
    column_count = sqlite3_column_count(statement);
    EXTEND_DATASET_DIRECT(2);
    append_to_dataset(2, dataset, term_count, ERL_DRV_PORT, driver_mk_port(drv->port));

    for (i = 0; i < column_count; i++) {
      LOG_DEBUG("Column %d type: %d\n", i, sqlite3_column_type(statement, i));
      switch (sqlite3_column_type(statement, i)) {
      case SQLITE_INTEGER: {
        ErlDrvSInt64 *int64_ptr = driver_alloc(sizeof(ErlDrvSInt64));
        *int64_ptr = (ErlDrvSInt64) sqlite3_column_int64(statement, i);
        ptrs = add_to_ptr_list(ptrs, int64_ptr);

        EXTEND_DATASET_DIRECT(2);
        append_to_dataset(2, dataset, term_count, ERL_DRV_INT64, (ErlDrvTermData) int64_ptr);
        break;
      }
      case SQLITE_FLOAT: {
        double *float_ptr = driver_alloc(sizeof(double));
        *float_ptr = sqlite3_column_double(statement, i);
        ptrs = add_to_ptr_list(ptrs, float_ptr);

        EXTEND_DATASET_DIRECT(2);
        append_to_dataset(2, dataset, term_count, ERL_DRV_FLOAT, (ErlDrvTermData) float_ptr);
        break;
      }
      case SQLITE_BLOB: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        binaries = add_to_ptr_list(binaries, binary);

        EXTEND_DATASET_DIRECT(8);
        append_to_dataset(8, dataset, term_count,
          ERL_DRV_ATOM, drv->atom_blob,
          ERL_DRV_BINARY, (ErlDrvTermData) binary, (ErlDrvTermData) bytes, (ErlDrvTermData) 0,
          ERL_DRV_TUPLE, (ErlDrvTermData) 2);
        break;
      }
      case SQLITE_TEXT: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        binaries = add_to_ptr_list(binaries, binary);

        EXTEND_DATASET_DIRECT(4);
        append_to_dataset(4, dataset, term_count,
          ERL_DRV_BINARY, (ErlDrvTermData) binary, (ErlDrvTermData) bytes, (ErlDrvTermData) 0);
        break;
      }
      case SQLITE_NULL: {
        EXTEND_DATASET_DIRECT(2);
        append_to_dataset(2, dataset, term_count, ERL_DRV_ATOM, drv->atom_null);
        break;
      }
      }
    }
    EXTEND_DATASET_DIRECT(2);
    append_to_dataset(2, dataset, term_count, ERL_DRV_TUPLE, (ErlDrvTermData) column_count);

    async_command->ptrs = ptrs;
    async_command->binaries = binaries;
    break;
  case SQLITE_DONE:
    EXTEND_DATASET_DIRECT(4);
    append_to_dataset(4, dataset, term_count,
      ERL_DRV_PORT, driver_mk_port(drv->port),
      ERL_DRV_ATOM, drv->atom_done);
    sqlite3_reset(statement);
    break;
  case SQLITE_BUSY:
    return_error(drv, SQLITE_BUSY, "SQLite3 database is busy",
                 &dataset, &term_count, &term_allocated,
                 &async_command->error_code);
    sqlite3_reset(statement);
    goto POPULATE_COMMAND;
    break;
  default:
    return_error(drv, result, sqlite3_errmsg(drv->db),
                 &dataset, &term_count, &term_allocated,
                 &async_command->error_code);
    sqlite3_reset(statement);
    goto POPULATE_COMMAND;
  }

  EXTEND_DATASET_DIRECT(2);
  append_to_dataset(2, dataset, term_count, ERL_DRV_TUPLE, (ErlDrvTermData) 2);

POPULATE_COMMAND:
  async_command->dataset = dataset;
  async_command->term_count = term_count;
  async_command->ptrs = ptrs;
  async_command->binaries = binaries;
  async_command->row_count = 1;
  LOG_DEBUG("Total term count: %p %d, columns count: %d\n", statement, term_count, column_count);
}

static void ready_async(ErlDrvData drv_data, ErlDrvThreadData thread_data) {
  async_sqlite3_command *async_command = (async_sqlite3_command *) thread_data;
  sqlite3_drv_t *drv = async_command->driver_data;

  int res =
    #ifdef PRE_R16B
    driver_output_term(drv->port,
    #else
    erl_drv_output_term(driver_mk_port(drv->port),
    #endif
      async_command->dataset,
      async_command->term_count);
  if (res != 1) {
    LOG_DEBUG("driver_output_term returned %d\n", res);
#ifdef DEBUG
    fprint_dataset(drv->log, async_command->dataset, async_command->term_count);
#endif
  }

  LOG_DEBUG("Total term count: %p %d, rows count: %d (%d)\n", async_command->statement, async_command->term_count, async_command->row_count, res);
  sql_free_async(async_command);
}

static int prepare(sqlite3_drv_t *drv, char *command, int command_size) {
  int result;
  const char *rest;
  sqlite3_stmt *statement;
  ErlDrvTermData spec[6];

  LOG_DEBUG("Preparing statement: %.*s\n", command_size, command);
  result = sqlite3_prepare_v2(drv->db, command, command_size, &statement, &rest);
  if (result != SQLITE_OK) {
    return output_db_error(drv);
  } else if (statement == NULL) {
    return output_error(drv, SQLITE_MISUSE, "empty statement");
  }

  if (drv->prepared_count >= drv->prepared_alloc) {
    drv->prepared_alloc =
      (drv->prepared_alloc != 0) ? 2*drv->prepared_alloc : 4;
    drv->prepared_stmts =
      driver_realloc(drv->prepared_stmts,
                     drv->prepared_alloc * sizeof(sqlite3_stmt *));
  }
  drv->prepared_stmts[drv->prepared_count] = statement;
  drv->prepared_count++;

  spec[0] = ERL_DRV_PORT;
  spec[1] = driver_mk_port(drv->port);
  spec[2] = ERL_DRV_UINT;
  spec[3] = drv->prepared_count - 1;
  spec[4] = ERL_DRV_TUPLE;
  spec[5] = 2;
  return
    #ifdef PRE_R16B
    driver_output_term(drv->port,
    #else
    erl_drv_output_term(spec[1],
    #endif
      spec, sizeof(spec) / sizeof(spec[0]));
}

static int prepared_bind(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  int result;
  unsigned int prepared_index;
  long long_prepared_index;
  int index = 0, type, size;
  sqlite3_stmt *statement;

  LOG_DEBUG("Finalizing prepared statement: %.*s\n", buffer_size, buffer);

  ei_decode_version(buffer, &index, NULL);
  ei_decode_tuple_header(buffer, &index, &size);
  // assert(size == 2);
  ei_decode_long(buffer, &index, &long_prepared_index);
  prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to bind non-existent prepared statement");
  }

  statement = drv->prepared_stmts[prepared_index];
  result =
    bind_parameters(drv, buffer, buffer_size, &index, statement, &type, &size);
  if (result == SQLITE_OK) {
    return output_ok(drv);
  } else {
    return result; // error has already been output
  }
}

static int prepared_columns(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  unsigned int prepared_index;
  long long_prepared_index;
  int index = 0, term_count = 0, term_allocated = 0, column_count;
  sqlite3_stmt *statement;
  ErlDrvTermData *dataset = NULL, port;
  ptr_list* ptrs = NULL;

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    LOG_DEBUG("Tried to get columns for prepared statement #%d, but maximum possible is #%d\n", prepared_index, drv->prepared_count - 1);

    return output_error(drv, SQLITE_MISUSE,
                        "Trying to reset non-existent prepared statement");
  }

  LOG_DEBUG("Getting the columns for prepared statement #%d\n", prepared_index);

  statement = drv->prepared_stmts[prepared_index];

  port = driver_mk_port(drv->port);
  EXTEND_DATASET_DIRECT(2);
  append_to_dataset(2, dataset, term_count, ERL_DRV_PORT, port);

  column_count = sqlite3_column_count(statement);

  get_columns(
    drv, statement, column_count, 2, &term_count, &term_allocated, &ptrs, &dataset);
  EXTEND_DATASET_DIRECT(2);
  append_to_dataset(2, dataset, term_count, ERL_DRV_TUPLE, (ErlDrvTermData) 2);

  #ifdef PRE_R16B
  driver_output_term(drv->port,
  #else
  erl_drv_output_term(port,
  #endif
    dataset, term_count);
  free_ptr_list(ptrs, driver_free_fun);
  driver_free(dataset);
  return 0;
}

static int prepared_step(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  unsigned int prepared_index;
  long long_prepared_index;
  int index = 0;
  sqlite3_stmt *statement;
  async_sqlite3_command *async_command;

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    LOG_DEBUG("Tried to make a step in prepared statement #%d, but maximum possible is #%d\n", prepared_index, drv->prepared_count - 1);
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to evaluate non-existent prepared statement");
  }

  LOG_DEBUG("Making a step in prepared statement #%d\n", prepared_index);

  statement = drv->prepared_stmts[prepared_index];
  async_command = make_async_command_statement(drv, statement, 0);

  exec_async_command(drv, sql_step_async, async_command);
  return 0;
}

static int prepared_reset(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  unsigned int prepared_index;
  long long_prepared_index;
  int index = 0;
  sqlite3_stmt *statement;

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    LOG_DEBUG("Tried to reset prepared statement #%d, but maximum possible is #%d\n",
      prepared_index, drv->prepared_count - 1);
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to reset non-existent prepared statement");
  }

  LOG_DEBUG("Resetting prepared statement #%d\n", prepared_index);
  // don't bother about error code, any errors should already be shown by step
  statement = drv->prepared_stmts[prepared_index];
  sqlite3_reset(statement);
  return output_ok(drv);
}

static int prepared_clear_bindings(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  unsigned int prepared_index;
  long long_prepared_index;
  int index = 0;
  sqlite3_stmt *statement;

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    LOG_DEBUG("Tried to clear bindings of prepared statement #%d, but maximum possible is #%d\n", prepared_index, drv->prepared_count - 1);
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to clear bindings of non-existent prepared statement");
  }

  LOG_DEBUG("Clearing bindings of prepared statement #%d\n", prepared_index);
  statement = drv->prepared_stmts[prepared_index];
  sqlite3_clear_bindings(statement);
  return output_ok(drv);
}

static int prepared_finalize(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  unsigned int prepared_index;
  long long_prepared_index;
  int index = 0;

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    LOG_DEBUG("Tried to finalize prepared statement #%d, but maximum possible is #%d\n",
      prepared_index, drv->prepared_count - 1);
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to finalize non-existent prepared statement");
  }

  LOG_DEBUG("Finalizing prepared statement #%d\n", prepared_index);
  // finalize the statement and make sure it isn't accidentally executed again
  sqlite3_finalize(drv->prepared_stmts[prepared_index]);
  drv->prepared_stmts[prepared_index] = NULL;

  // if the statement is at the end of the array, space can be reused;
  // otherwise don't bother
  if (prepared_index == drv->prepared_count - 1) {
    drv->prepared_count--;
  }
  return output_ok(drv);
}

// Unknown Command
static int unknown(sqlite3_drv_t *drv, char *command, int command_size) {
  // Return {Port, error, -1, unknown_command}
  ErlDrvTermData spec[] = {
    ERL_DRV_PORT, driver_mk_port(drv->port),
    ERL_DRV_ATOM, drv->atom_error,
    ERL_DRV_INT, (ErlDrvTermData) ((ErlDrvSInt) -1),
    ERL_DRV_ATOM, drv->atom_unknown_cmd,
    ERL_DRV_TUPLE, 4
  };
  return
    #ifdef PRE_R16B
    driver_output_term(drv->port,
    #else
    erl_drv_output_term(spec[1],
    #endif
      spec, sizeof(spec) / sizeof(spec[0]));
}

#ifdef DEBUG
static void fprint_dataset(FILE* log, ErlDrvTermData *dataset, int term_count) {
  int i = 0, stack_size = 0;
  ErlDrvUInt length;

  fprintf(log, "\nPrinting dataset\n");
  while(i < term_count) {
    switch (dataset[i]) {
    case ERL_DRV_NIL:
      fprintf(log, "%d: []", i);
      i++;
      stack_size++;
      break;
    case ERL_DRV_ATOM:
      fprintf(log, "%d-%d: an atom", i, i+1);
      i += 2;
      stack_size++;
      break;
    case ERL_DRV_INT:
      fprintf(log, "%d-%d: int %ld", i, i+1, (ErlDrvSInt) dataset[i+1]);
      i += 2;
      stack_size++;
      break;
    case ERL_DRV_PORT:
      fprintf(log, "%d-%d: a port", i, i+1);
      i += 2;
      stack_size++;
      break;
    case ERL_DRV_BINARY:
      fprintf(log, "%d-%d: a binary (length %lu, offset %lu)",
             i, i+3, (ErlDrvUInt) dataset[i+2], (ErlDrvUInt) dataset[i+3]);
      i += 4;
      stack_size++;
      break;
    case ERL_DRV_BUF2BINARY:
      fprintf(log, "%d-%d: a string used as binary (length %lu)", i, i+2, (ErlDrvUInt) dataset[i+2]);
      i += 3;
      stack_size++;
      break;
    case ERL_DRV_STRING:
      fprintf(log, "%d-%d: a string (length %lu)", i, i+2, (ErlDrvUInt) dataset[i+2]);
      i += 3;
      stack_size++;
      break;
    case ERL_DRV_TUPLE:
      length = (ErlDrvUInt) dataset[i+1];
      fprintf(log, "%d-%d: a tuple (size %lu)", i, i+1, length);
      i += 2;
      stack_size -= length - 1;
      break;
    case ERL_DRV_LIST:
      length = (ErlDrvUInt) dataset[i+1];
      fprintf(log, "%d-%d: a list (length %lu)", i, i+1, length);
      i += 2;
      stack_size -= length - 1;
      break;
    case ERL_DRV_PID:
      fprintf(log, "%d-%d: a pid", i, i+1);
      i += 2;
      stack_size++;
      break;
    case ERL_DRV_STRING_CONS:
      length = (ErlDrvUInt) dataset[i+2];
      fprintf(log, "%d-%d: a string inside surrounding list (length %lu)", i, i+2, length);
      i += 3;
      break;
    case ERL_DRV_FLOAT:
      fprintf(log, "%d-%d: float %f", i, i+1, (double) dataset[i+1]);
      i += 2;
      stack_size++;
      break;
    case ERL_DRV_EXT2TERM:
      fprintf(log, "%d-%d: a term in external format of length %lu", i, i+1, (ErlDrvUInt) dataset[i+1]);
      i += 2;
      stack_size++;
      break;
    case ERL_DRV_INT64:
#if defined(_MSC_VER)
      fprintf(log, "%d-%d: int %I64d", i, i+1, (ErlDrvSInt64) dataset[i+1]);
#else
      fprintf(log, "%d-%d: int %lld", i, i+1, (ErlDrvSInt64) dataset[i+1]);
#endif
      i += 2;
      stack_size++;
      break;
    case ERL_DRV_UINT64:
#if defined(_MSC_VER)
      fprintf(log, "%d-%d: int %I64lu", i, i+1, (ErlDrvUInt64) dataset[i+1]);
#else
      fprintf(log, "%d-%d: int %llu", i, i+1, (ErlDrvUInt64) dataset[i+1]);
#endif
      i += 2;
      stack_size++;
      break;
    default:
      fprintf(log, "%d: unexpected type", i);
      i++;
      break;
    }
    fprintf(log, ".\tStack size: %d\n", stack_size);
    fflush(log);
  }
}
#endif
