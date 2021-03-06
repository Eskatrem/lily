/*  mod_lily.c
    This is an apache binding for the Lily language. */
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"
#include "util_script.h"

#include "lily_parser.h"
#include "lily_utf8.h"

#include "lily_api_hash.h"
#include "lily_api_alloc.h"
#include "lily_api_value_ops.h"
#include "lily_api_options.h"

struct table_bind_data {
    lily_hash_val *hash_val;
    const char *sipkey;
    uint16_t cid_tainted;
};

#define CID_TAINTED 0

lily_value *bind_tainted_of(lily_value *input, uint16_t cid_tainted)
{
    lily_instance_val *iv = lily_new_instance_val();
    iv->values = lily_malloc(1 * sizeof(lily_value *));
    iv->instance_id = cid_tainted;
    iv->values[0] = input;
    lily_value *result = lily_new_empty_value();
    lily_move_instance_f(MOVE_DEREF_NO_GC, result, iv);
    return result;
}

extern uint64_t siphash24(const void *src, unsigned long src_sz, const char key[16]);

/* This is temporary. I've added this because I don't want to 'fix' the hash api
   when it's going to be removed soon. */
static void apache_add_unique_hash_entry(const char *sipkey,
        lily_hash_val *hash_val, lily_value *pair_key, lily_value *pair_value)
{
    lily_hash_elem *elem = lily_malloc(sizeof(lily_hash_elem));

    uint64_t key_siphash = siphash24(pair_key->value.string->string,
            pair_key->value.string->size, sipkey);

    elem->key_siphash = key_siphash;
    elem->elem_key = pair_key;
    elem->elem_value = pair_value;

    if (hash_val->elem_chain)
        hash_val->elem_chain->prev = elem;

    elem->prev = NULL;
    elem->next = hash_val->elem_chain;
    hash_val->elem_chain = elem;

    hash_val->num_elems++;
}

static int bind_table_entry(void *data, const char *key, const char *value)
{
    /* Don't allow anything to become a string that has invalid utf-8, because
       Lily's string type assumes valid utf-8. */
    if (lily_is_valid_utf8(key) == 0 ||
        lily_is_valid_utf8(value) == 0)
        return TRUE;

    struct table_bind_data *d = data;

    lily_value *elem_key = lily_new_string(key);
    lily_value *elem_raw_value = lily_new_string(value);
    lily_value *elem_value = bind_tainted_of(elem_raw_value, d->cid_tainted);

    apache_add_unique_hash_entry(d->sipkey, d->hash_val, elem_key, elem_value);
    return TRUE;
}

static lily_value *bind_table_as(lily_options *options, apr_table_t *table,
        uint16_t *cid_table, char *name)
{
    lily_value *v = lily_new_empty_value();
    lily_move_hash_f(MOVE_DEREF_NO_GC, v, lily_new_hash_val());

    struct table_bind_data data;
    data.cid_tainted = cid_table[CID_TAINTED];
    data.hash_val = v->value.hash;
    data.sipkey = options->sipkey;
    apr_table_do(bind_table_entry, &data, table, NULL);
    return v;
}

static lily_value *bind_post(lily_options *options, uint16_t *cid_table)
{
    lily_value *v = lily_new_empty_value();
    lily_move_hash_f(MOVE_DEREF_NO_GC, v, lily_new_hash_val());
    lily_hash_val *hash_val = v->value.hash;
    uint16_t cid_tainted = cid_table[CID_TAINTED];
    request_rec *r = (request_rec *)options->data;

    apr_array_header_t *pairs;
    apr_off_t len;
    apr_size_t size;
    char *buffer;

    /* Credit: I found out how to use this by reading httpd 2.4's mod_lua
       (specifically req_parsebody of lua_request.c). */
    int res = ap_parse_form_data(r, NULL, &pairs, -1, 1024 * 8);
    if (res == OK) {
        while (pairs && !apr_is_empty_array(pairs)) {
            ap_form_pair_t *pair = (ap_form_pair_t *) apr_array_pop(pairs);
            if (lily_is_valid_utf8(pair->name) == 0)
                continue;

            apr_brigade_length(pair->value, 1, &len);
            size = (apr_size_t) len;
            buffer = lily_malloc(size + 1);

            if (lily_is_valid_utf8(buffer) == 0) {
                lily_free(buffer);
                continue;
            }

            apr_brigade_flatten(pair->value, buffer, &size);
            buffer[len] = 0;

            lily_value *elem_key = lily_new_string(pair->name);
            /* Give the buffer to the value to save memory. */
            lily_value *elem_raw_value = lily_new_string_take(buffer);
            lily_value *elem_value = bind_tainted_of(elem_raw_value,
                    cid_tainted);

            apache_add_unique_hash_entry(options->sipkey, hash_val, elem_key,
                    elem_value);
        }
    }

    return v;
}

static lily_value *bind_get(lily_options *options, uint16_t *cid_table)
{
    apr_table_t *http_get_args;
    ap_args_to_table((request_rec *)options->data, &http_get_args);

    return bind_table_as(options, http_get_args, cid_table, "get");
}

static lily_value *bind_env(lily_options *options, uint16_t *cid_table)
{
    request_rec *r = (request_rec *)options->data;
    ap_add_cgi_vars(r);
    ap_add_common_vars(r);

    return bind_table_as(options, r->subprocess_env, cid_table, "env");
}

static lily_value *bind_httpmethod(lily_options *options)
{
    lily_value *v = lily_new_empty_value();
    request_rec *r = (request_rec *)options->data;

    lily_move_string(v, lily_new_raw_string(r->method));
    return v;
}

/*  Implements server.write_literal

    This writes a literal directly to the server, with no escaping being done.
    If the value provided is not a literal, then ValueError is raised. */
void lily_apache_server_write_literal(lily_vm_state *vm, uint16_t argc, uint16_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *write_reg = vm_regs[code[1]];
    if (write_reg->flags & VAL_IS_DEREFABLE)
        lily_vm_raise(vm, SYM_CLASS_VALUEERROR,
                "The string passed must be a literal.\n");

    char *value = write_reg->value.string->string;

    ap_rputs(value, (request_rec *)vm->data);
}

/*  Implements server.write_raw

    This function takes a string and writes it directly to the server. It is
    assumed that escaping has already been done by server.escape. */
void lily_apache_server_write_raw(lily_vm_state *vm, uint16_t argc, uint16_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    char *value = vm_regs[code[1]]->value.string->string;

    ap_rputs(value, (request_rec *)vm->data);
}

extern void lily_string_html_encode(lily_vm_state *, uint16_t, uint16_t *);
extern int lily_maybe_html_encode_to_buffer(lily_vm_state *, lily_value *);

/*  Implements server.write

    This function takes a string and creates a copy with html encoding performed
    upon it. The resulting string is then sent to the server. */
void lily_apache_server_write(lily_vm_state *vm, uint16_t argc, uint16_t *code)
{
    lily_value *input = vm->vm_regs[code[1]];
    const char *source;

    /* String.html_encode can't be called directly, for a couple reasons.
       1: It expects a result register, and there isn't one.
       2: It may create a new String, which is unnecessary. */
    if (lily_maybe_html_encode_to_buffer(vm, input) == 0)
        source = input->value.string->string;
    else
        source = vm->vm_buffer->message;

    ap_rputs(source, (request_rec *)vm->data);
}

/*  Implements server.escape

    This function takes a string and performs basic html encoding upon it. The
    resulting string is safe to pass to server.write_raw. */
void lily_apache_server_escape(lily_vm_state *vm, uint16_t argc, uint16_t *code)
{
    lily_string_html_encode(vm, argc, code);
}

#define SERVER_ESCAPE        1
#define SERVER_WRITE_RAW     2
#define SERVER_WRITE_LITERAL 3
#define SERVER_WRITE         4
#define VAR_HTTPMETHOD       5
#define VAR_POST             6
#define VAR_GET              7
#define VAR_ENV              8

void *lily_apache_loader(lily_options *options, uint16_t *cid_table, int id)
{
    switch (id) {
        case SERVER_ESCAPE:        return lily_apache_server_escape;
        case SERVER_WRITE_RAW:     return lily_apache_server_write_raw;
        case SERVER_WRITE_LITERAL: return lily_apache_server_write_literal;
        case SERVER_WRITE:         return lily_apache_server_write;
        case VAR_HTTPMETHOD:       return bind_httpmethod(options);
        case VAR_POST:             return bind_post(options, cid_table);
        case VAR_GET:              return bind_get(options, cid_table);
        case VAR_ENV:              return bind_env(options, cid_table);
        default:                   return NULL;
    }
}

const char *dl_table[] =
{
    "\001Tainted"
    ,"F\000escape\0(String):String"
    ,"F\000write_raw\0(String)"
    ,"F\000write_literal\0(String)"
    ,"F\000write\0(String)"
    ,"R\000httpmethod\0String"
    ,"R\000post\0Hash[String, Tainted[String]]"
    ,"R\000get\0Hash[String, Tainted[String]]"
    ,"R\000env\0Hash[String, Tainted[String]]"
    ,"Z"
};

static int lily_handler(request_rec *r)
{
    if (strcmp(r->handler, "lily"))
        return DECLINED;

    r->content_type = "text/html";

    lily_options *options = lily_new_default_options();
    options->data = r;
    options->html_sender = (lily_html_sender) ap_rputs;

    lily_parse_state *parser = lily_new_parse_state(options);
    lily_register_package(parser, "server", dl_table, lily_apache_loader);

    lily_parse_file(parser, lm_tags, r->filename);

    lily_free_parse_state(parser);
    lily_free_options(options);

    return OK;
}

static void lily_register_hooks(apr_pool_t *p)
{
    ap_hook_handler(lily_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA lily_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                  /* create per-dir    config structures */
    NULL,                  /* merge  per-dir    config structures */
    NULL,                  /* create per-server config structures */
    NULL,                  /* merge  per-server config structures */
    NULL,                  /* table of config file commands       */
    lily_register_hooks    /* register hooks                      */
};

