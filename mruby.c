/**********************
 * 
 * MRuby Plugin for HexChat
 * 
 * See README.md
 * 
 **********************/

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif

#include "hexchat-plugin.h"
#undef _POSIX_C_SOURCE  /* Avoid warnings from /usr/include/features.h */
#undef _XOPEN_SOURCE
#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/string.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/array.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/dump.h>

// This contains the Ruby code that provides the high-level interface
#include "hexchat_mrb_lib.h"

static hexchat_plugin *ph;                /* plugin handle */
static mrb_state *hex_g_mrb;              /* mruby interpreter state */
static mrbc_context *console_cxt = NULL;  /* console context */
static struct RClass *hexchat_module;     /* HexChat module */
static struct RClass *internal_class;     /* HexChat::Internal class */
static struct RClass *cxt_class;          /* HexChat::Internal::Context class */
static struct RClass *list_class;         /* HexChat::Internal::List class */
static struct RClass *hook_class;         /* HexChat::Internal::Hook class */

// This structure holds a HexChat context pointer
// We will wrap this as an instance of class HexChat::Internal::Context
struct mrb_hexchat_context {
  hexchat_context *c;   /* HexChat context */
};

// This structure holds a HexChat list pointer
// We will wrap this as ab instance of class HexChat::Internal::List
struct mrb_hexchat_list {
  hexchat_list *l;      /* HexChat list */
};

// This structure holds a map of ruby code to HexChat hooks
// We will wrap this as an instance of class HexChat::Internal::Hook
struct mrb_hexchat_hook {
  mrb_state *mrb;       /* MRuby interpreter state */
  void *xhook;          /* HexChat hook handle */
  mrb_value block;      /* Ruby block reference */
  mrb_value ref;        /* Object reference */
  /* Object reference is used to provide access to the containing object
  * Normally, this is an instance of HexChat::Hook, which provides the
  * high-level interface to hooks.  This is then used to set $__HOOK_REF
  * when the hook is called.  Finally, this provides a means to unhook
  * hooks from within the called block.  This makes one-shot timers possible
  * or one-time commands, etc. */
};

// prototype for freeing an allocated hook
static void
hex_mrb_hook_free(mrb_state *mrb, struct mrb_hexchat_hook *hk);

// MRuby data type structures
static const struct mrb_data_type mrb_hexchat_cxt_type = {
  "HexChat::Internal::Context", mrb_free
};

static const struct mrb_data_type mrb_hexchat_list_type = {
  "HexChat::Internal::List", mrb_free
};

static const struct mrb_data_type mrb_hexchat_hook_type = {
  "HexChat::Internal::Hook", (void *)hex_mrb_hook_free
};

// "File names" for varous execution contexts
static const char *mrb_file_internal = "(internal)";
static const char *mrb_file_eval     = "(eval)";
static const char *mrb_file_console  = "(console)";
static const char *mrb_file_none     = "(none)";

// Allocate a MRuby HexChat context data type
static struct mrb_hexchat_context*
hex_mrb_context_alloc(mrb_state *mrb, hexchat_context *c)
{
  struct mrb_hexchat_context *cxt;
  cxt = (struct mrb_hexchat_context *)mrb_malloc(mrb, sizeof(struct mrb_hexchat_context));
  cxt->c = c;
  return cxt;
}

// Wrap the mrb_hexchat_context structure
static mrb_value
hex_mrb_context_wrap(mrb_state *mrb, struct RClass *cc, struct mrb_hexchat_context *cxt)
{
  return mrb_obj_value(Data_Wrap_Struct(mrb, cc, &mrb_hexchat_cxt_type, cxt));
}

// Allocate a MRuby HexChat list data type
static struct mrb_hexchat_list*
hex_mrb_list_alloc(mrb_state *mrb, hexchat_list *l)
{
  struct mrb_hexchat_list *lst;
  lst = (struct mrb_hexchat_list *)mrb_malloc(mrb, sizeof(struct mrb_hexchat_list));
  lst->l = l;
  return lst;
}

// Wrap the mrb_hexchat_list structure
static mrb_value
hex_mrb_list_wrap(mrb_state *mrb, struct RClass *lc, struct mrb_hexchat_list *lst)
{
  return mrb_obj_value(Data_Wrap_Struct(mrb, lc, &mrb_hexchat_list_type, lst));
}

// Wrap the mrb_hexchat_hook structure
//static mrb_value
//mrb_hexchat_hook_wrap(mrb_state *mrb, struct RClass *hc, struct mrb_hexchat_hook *hk)
//{
//  return mrb_obj_value(Data_Wrap_Struct(mrb, hc, &mrb_hexchat_hook_type, hk));
//}

// Allocate an mrb_hexchat_hook object and initialize
static struct mrb_hexchat_hook*
hex_mrb_hook_alloc(mrb_state *mrb, mrb_value block)
{
  struct mrb_hexchat_hook *hk;
  hk = (struct mrb_hexchat_hook *)mrb_malloc(mrb, sizeof(struct mrb_hexchat_hook));
  hk->xhook = NULL;
  hk->mrb = mrb;
  hk->block = block;
  hk->ref = mrb_nil_value();
  mrb_gc_register(mrb, hk->block);	// Prevent MRuby from GC the block
  return hk;
}

// Unhook a hook referenced in an mrb_hexchat_hook structure
static void
hex_mrb_hook_unhook(struct mrb_hexchat_hook *hk)
{
  if (hk->xhook != NULL) {
    hexchat_unhook(ph, hk->xhook);
    // printf("MRB unhooked %p (xchat %p) from %llx\n", (void *)hk, (void *)hk->xhook, (unsigned long long int)mrb_obj_id(hk->block));
    hk->xhook = NULL;
  }
}

// I need to see if MRuby has this check already
// if so, next two functions are redundant.
static void
hex_mrb_gc_register_if_not_nil(mrb_state *mrb, mrb_value value)
{
  if (!mrb_obj_equal(mrb, value, mrb_nil_value())) {
    mrb_gc_register(mrb, value);
  }
}

static void
hex_mrb_gc_unregister_if_not_nil(mrb_state *mrb, mrb_value value)
{
  if (!mrb_obj_equal(mrb, value, mrb_nil_value())) {
    mrb_gc_unregister(mrb, value);
  }
}

// Free a mrb_hexchat_hook structure
static void
hex_mrb_hook_free(mrb_state *mrb, struct mrb_hexchat_hook *hk)
{
  hex_mrb_hook_unhook(hk);
  mrb_gc_unregister(mrb, hk->block);
  hex_mrb_gc_unregister_if_not_nil(mrb, hk->ref);
  mrb_free(mrb, hk);
}

// Set the object reference in an mrb_hexchat_hook
static mrb_value
hex_mrb_hook_set_ref(mrb_state *mrb, struct mrb_hexchat_hook *hk,  mrb_value ref)
{
  mrb_value old_ref = hk->ref;
  hex_mrb_gc_unregister_if_not_nil(mrb, hk->ref);
  hk->ref = ref;
  hex_mrb_gc_register_if_not_nil(mrb, hk->ref);
  return old_ref;
}

// Put a HexChat hook into an mrb_hexchat_hook data structure
static void
hex_mrb_hook_hook(mrb_state *mrb, struct mrb_hexchat_hook *hk, void *xhook)
{
  hex_mrb_hook_unhook(hk);
  hk->xhook = xhook;
  hk->mrb = mrb;
  // printf("MRB hooked %p (xchat %p) to %llx\n", (void *)hk, (void *)xhook, (unsigned long long  int)mrb_obj_id(hk->block));
}

// Convert HexChat "words" to an MRuby Array
static mrb_value
hex_mrb_words_to_array(mrb_state *mrb, char *word[], int start, int limit)
{
  mrb_value array = mrb_ary_new(mrb);
  if (word == NULL) {
    return array;
  }
  for (
      int index = start;
      index < limit && word[index] != NULL && word[index][0] != 0;
      index++
  ) {
    mrb_value value = mrb_str_new_cstr(mrb, word[index]);
    mrb_ary_push(mrb, array, value);
  }
  return array;
}

// Return string representation of a C pointer
static mrb_value
hex_mrb_ptr_to_s(mrb_state *mrb, void *ptr)
{
  mrb_value result = mrb_nil_value();
  if (ptr != NULL) {
    char buf[32];
    snprintf(buf, 31, "%p", ptr);
    result = mrb_str_new_cstr(mrb, buf);
  }
  return result;
}

// Return hex string of Ruby object ID, can probably refactor this out
static mrb_value
hex_mrb_obj_id_to_s(mrb_state *mrb, mrb_value v)
{
  char buf[32];
  snprintf(buf, 31, "%llx", (unsigned long long int)mrb_obj_id(v));
  return mrb_str_new_cstr(mrb, buf);
}

// Print an MRuby array in HexChat
static void
hex_mrb_print_array(mrb_state *mrb, mrb_value array)
{
  mrb_int l = mrb_ary_len(mrb, array);
  //printf("%lld lines in array\n", (long long int)l);
  for (mrb_int i = 0; i < l; ++i) {
    mrb_value v = mrb_ary_ref(mrb, array, i);
    char *s = mrb_str_to_cstr(mrb, v);
    hexchat_print(ph, s);
  }
}

// Print current MRuby exception in HexChat
static void
hex_mrb_print_exc(mrb_state *mrb)
{
  if (mrb->exc) {
    mrb_value e = mrb_obj_value(mrb->exc);
    if (mrb_obj_is_kind_of(mrb, e, E_SYSSTACK_ERROR)) {
      mrb_value i = mrb_inspect(mrb, e);
      hexchat_print(ph, mrb_str_to_cstr(mrb, i));
      hexchat_print(ph, "Backtrace suppressed due to stack overflow!");
    } else {
      mrb_value t = mrb_exc_backtrace(mrb, e);
      mrb_value i = mrb_inspect(mrb, e);
      hexchat_print(ph, mrb_str_to_cstr(mrb, i));
      hex_mrb_print_array(mrb, t);
    }
  } else {
    hexchat_print(ph, "No exception!");
  }
}

// Get data from hook C structure into an array
static mrb_value
hex_mrb_hook_info(mrb_state *mrb, struct mrb_hexchat_hook *hk)
{
  mrb_value array = mrb_ary_new(mrb);
  mrb_ary_push(mrb, array, hex_mrb_ptr_to_s(mrb, hk));
  mrb_ary_push(mrb, array, hex_mrb_ptr_to_s(mrb, hk->xhook));
  mrb_ary_push(mrb, array, hex_mrb_obj_id_to_s(mrb, hk->block));
  mrb_ary_push(mrb, array, mrb_fixnum_value(mrb_obj_id(hk->block)));
  return array;
}

// HexChat::Internal.print(String)
static mrb_value
hex_mrb_xi_print(mrb_state *mrb, mrb_value self)
{
  char *out;
  mrb_get_args(mrb, "z", &out);
  hexchat_print(ph, out);
  return mrb_nil_value();
}

// HexChat::Internal.command(String)
static mrb_value
hex_mrb_xi_command(mrb_state *mrb, mrb_value self)
{
  char *cmd;
  mrb_get_args(mrb, "z", &cmd);
  hexchat_command(ph, cmd);
  return mrb_nil_value();
}

// HexChat::Internal.get_info(String)
static mrb_value
hex_mrb_xi_get_info(mrb_state *mrb, mrb_value self)
{
  char *id;
  const char *info;
  mrb_value result = mrb_nil_value();
  mrb_get_args(mrb, "z", &id);
  info = hexchat_get_info(ph, id);
  if (info != NULL) {
    result = mrb_str_new_cstr(mrb, info);
  }
  return result;
}

// HexChat::Internal.strip(String)
static mrb_value
hex_mrb_xi_strip(mrb_state *mrb, mrb_value self)
{
  const char *text;
  int flags = 1 | 2;
  char *new_text;
  mrb_value result = mrb_nil_value();
  mrb_get_args(mrb, "z|i", &text, &flags);
  new_text = hexchat_strip(ph, text, -1, flags);
  if (new_text != NULL) {
    result = mrb_str_new_cstr(mrb, new_text);
    hexchat_free(ph, new_text);
  }
  return result;
}

// HexChat::Internal.get_prefs(String)
static mrb_value
hex_mrb_xi_get_prefs(mrb_state *mrb, mrb_value self)
{
  const char *name;
  int r_type;
  int r_int;
  const char *r_str;
  mrb_value result = mrb_nil_value();
  mrb_get_args(mrb, "z", &name);
  r_type = hexchat_get_prefs(ph, name, &r_str, &r_int);
  switch(r_type) {
  case 1: // string
      result = mrb_str_new_cstr(mrb, r_str);
      break;
  case 2: // int
      result = mrb_fixnum_value((mrb_int)r_int);
      break;
  case 3: // bool
      if (r_int) {
        result = mrb_true_value();
      } else {
        result = mrb_false_value();
      }
      break;
  }
  return result;
}

// HexChat::Internal.pluginpref_set_str(String, String)
static mrb_value
hex_mrb_xi_pluginpref_set_str(mrb_state *mrb, mrb_value self)
{
  const char *var;
  const char *value;
  int success;
  mrb_get_args(mrb, "zz", &var, &value);
  success = hexchat_pluginpref_set_str(ph, var, value);
  return success ? mrb_true_value() : mrb_false_value();
}

// HexChat::Internal.pluginpref_get_str(String)
static mrb_value
hex_mrb_xi_pluginpref_get_str(mrb_state *mrb, mrb_value self)
{
  char buf[512];
  char *var;
  int success;
  mrb_value result = mrb_nil_value();
  mrb_get_args(mrb, "z", &var);
  success = hexchat_pluginpref_get_str(ph, var, buf);
  if (success) {
    result = mrb_str_new_cstr(mrb, buf);
  }
  return result;
}

// HexChat::Internal.pluginpref_set_int(String, Integer)
static mrb_value
hex_mrb_xi_pluginpref_set_int(mrb_state *mrb, mrb_value self)
{
  const char *var;
  int value;
  int success;
  mrb_get_args(mrb, "zi", &var, &value);
  success = hexchat_pluginpref_set_int(ph, var, value);
  return success ? mrb_true_value() : mrb_false_value();
}

// HexChat::Internal.pluginpref_get_int(String)
static mrb_value
hex_mrb_xi_pluginpref_get_int(mrb_state *mrb, mrb_value self)
{
  char *var;
  mrb_get_args(mrb, "z", &var);
  return mrb_fixnum_value((mrb_int)hexchat_pluginpref_get_int(ph, var));
}

// HexChat::Internal.nickcmp(String, String)
static mrb_value
hex_mrb_xi_nickcmp(mrb_state *mrb, mrb_value self)
{
  const char *nick1;
  const char *nick2;
  mrb_get_args(mrb, "zz", &nick1, &nick2);
  return mrb_fixnum_value((mrb_int)hexchat_nickcmp(ph, nick1, nick2));
}

// HexChat::Internal.emit_print(String, [String]...)
// (takes up to 6 strings after the required one)
static mrb_value
hex_mrb_xi_emit_print(mrb_state *mrb, mrb_value self)
{
  char *name;
  char *argv[6];
  int result;
  memset(&argv, 0, sizeof(char*)*6);
  mrb_get_args(mrb, "z|z!z!z!z!z!z!", &name,
          &argv[0], &argv[1], &argv[2],
          &argv[3], &argv[4], &argv[5]);
  result = hexchat_emit_print(ph, name,
          argv[0], argv[1], argv[2],
          argv[3], argv[4], argv[5], NULL);
  return result ? mrb_true_value() : mrb_false_value();
}

// HexChat::Internal.load(String)
// Loads an MRuby file
static mrb_value
hex_mrb_xi_load(mrb_state *mrb, mrb_value self) {
  char *fname;
  FILE *file;
  mrb_value result = mrb_nil_value();
  mrb_get_args(mrb, "z", &fname);
  file = fopen(fname, "rb");  // b flag for OSes that care...
  if (file != NULL) {
    char buf[4];
    size_t b_read;
    mrbc_context *c = mrbc_context_new(mrb);
    mrbc_filename(mrb, c, fname);
    c->lineno = 1;
    b_read = fread((void *)buf, 1, 4, file);
    rewind(file);
    if (b_read == 4 && strncmp("RITE", buf, 4) == 0) {
      // RiteVM compiled - load it
      mrb_load_irep_file_cxt(mrb, file, c);
    } else {
      // Reopen without binary flag and load it
      fclose(file);
      file = fopen(fname, "r");
      mrb_load_file_cxt(mrb, file, c);
    }
    fclose(file);
    mrbc_context_free(mrb, c);
    if (mrb->exc) {
      hexchat_printf(ph, "error loading %s", fname);
      hex_mrb_print_exc(mrb);
      mrb->exc = 0;
      result = mrb_false_value();
    } else {
      result = mrb_true_value();
    }
  }
  return result;
}

// HexChat::Internal::List.initialize(String)
static mrb_value
hex_mrb_xl_initialize(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst;
  hexchat_list *l;
  const char *name;
  lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  if (lst) {
          mrb_free(mrb, lst);
  }
  mrb_data_init(self, NULL, &mrb_hexchat_list_type);
  mrb_get_args(mrb, "z", &name);
  l = hexchat_list_get(ph, name);
  lst = hex_mrb_list_alloc(mrb, l);
  mrb_data_init(self, lst, &mrb_hexchat_list_type);
  return self;
}

// HexChat::Internal::List.get(String)
static mrb_value
hex_mrb_xl_get(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst;
  hexchat_list *l;
  const char *name;
  mrb_get_args(mrb, "z", &name);
  l = hexchat_list_get(ph, name);
  if (l == NULL) {
    return mrb_nil_value();
  }
  lst = hex_mrb_list_alloc(mrb, l);
  return hex_mrb_list_wrap(mrb, mrb_class_ptr(self), lst);
}

// HexChat::Internal::List#ptr
// Return string rep of C pointer to HexChat list
static mrb_value
hex_mrb_xl_ptr(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst;
  lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  return hex_mrb_ptr_to_s(mrb, (void *)lst->l);
}

// HexChat::Internal::List.fields
// Return array of fields in this list
static mrb_value
hex_mrb_xl_fields(mrb_state *mrb, mrb_value self)
{
  const char *name;
  const char *const *fields;
  mrb_get_args(mrb, "z", &name);
  fields = hexchat_list_fields(ph, name);
  //if (fields == NULL) {
  //  return mrb_nil_value();
  //}
  return hex_mrb_words_to_array(mrb, (char **)fields, 0, 32);
}

// HexChat::Internal::List#next
static mrb_value
hex_mrb_xl_next(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  int r = 0;
  if (lst->l == NULL) {
    return mrb_nil_value();
  } else {
    r = hexchat_list_next(ph, lst->l);
  }
  return r ? mrb_true_value() : mrb_false_value();
}

// HexChat::Internal::List#free
static mrb_value
hex_mrb_xl_free(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  if (lst->l != NULL) {
    hexchat_list_free(ph, lst->l);
    lst->l = NULL;
  }
  return mrb_nil_value();
}

// HexChat::Internal::List#free?
static mrb_value
hex_mrb_xl_free_q(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  mrb_value result = mrb_false_value();
  if (lst->l == NULL) {
    result = mrb_true_value();
  }
  return result;
}

// HexChat::Internal::List#str(String)
static mrb_value
hex_mrb_xl_str(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  mrb_value result = mrb_nil_value();
  char *name;
  mrb_get_args(mrb, "z", &name);
  if (lst->l != NULL) {
    const char *str;
    str = hexchat_list_str(ph, lst->l, name);
    if (str != NULL) {
      result = mrb_str_new_cstr(mrb, str);
    }
  }
  return result;
}

// HexChat::Internal::List#int(String)
static mrb_value
hex_mrb_xl_int(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  mrb_value result = mrb_nil_value();
  char *name;
  mrb_get_args(mrb, "z", &name);
  if (lst->l != NULL) {
    mrb_int i;
    i = (mrb_int)hexchat_list_int(ph, lst->l, name);
    result = mrb_fixnum_value(i);
  }
  return result;
}

// HexChat::Internal::List#cxt(String)
static mrb_value
hex_mrb_xl_cxt(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  mrb_value result = mrb_nil_value();
  char *name;
  mrb_get_args(mrb, "z", &name);
  if (lst->l != NULL) {
    const char *c;
    c = hexchat_list_str(ph, lst->l, name);
    if (c != NULL) {
      struct mrb_hexchat_context *cxt;
      cxt = hex_mrb_context_alloc(mrb, (hexchat_context *)c);
      result = hex_mrb_context_wrap(mrb, cxt_class, cxt);
    }
  }
  return result;
}

// HexChat::Internal::List#time(String)
static mrb_value
hex_mrb_xl_time(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_list *lst = (struct mrb_hexchat_list *)DATA_PTR(self);
  mrb_value result = mrb_nil_value();
  char *name;
  mrb_get_args(mrb, "z", &name);
  if (lst->l != NULL) {
    time_t t;
    t = hexchat_list_time(ph, lst->l, name);
    result = mrb_fixnum_value((mrb_int)t);
  }
  return result;
}

// HexChat::Internal::Context#current
static mrb_value
hex_mrb_xc_current(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_context *cxt;
  hexchat_context *c = hexchat_get_context(ph);
  cxt = hex_mrb_context_alloc(mrb, c);
  return hex_mrb_context_wrap(mrb, mrb_class_ptr(self), cxt);
}

// HexChat::Internal::Context#find(String, String)
static mrb_value
hex_mrb_xc_find(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_context *cxt;
  hexchat_context *c;
  char *serv = NULL;
  char *chan = NULL;
  mrb_get_args(mrb, "|z!z!", &serv, &chan);
  c = hexchat_find_context(ph, serv, chan);
  if (c == NULL) {
    return mrb_nil_value();
  }
  cxt = hex_mrb_context_alloc(mrb, c);
  return hex_mrb_context_wrap(mrb, mrb_class_ptr(self), cxt);
}

// HexChat::Internal::Context.initialize(String, String)
static mrb_value
hex_mrb_xc_initialize(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_context *cxt;
  hexchat_context *c;
  char *serv = NULL;
  char *chan = NULL;
  cxt = (struct mrb_hexchat_context *)DATA_PTR(self);
  if (cxt) {
    mrb_free(mrb, cxt);
  }
  mrb_data_init(self, NULL, &mrb_hexchat_cxt_type);
  mrb_get_args(mrb, "|z!z!", &serv, &chan);
  c = hexchat_find_context(ph, serv, chan);
  cxt = hex_mrb_context_alloc(mrb, c);
  mrb_data_init(self, cxt, &mrb_hexchat_cxt_type);
  return self;
}

// HexChat::Internal::Context#set
static mrb_value
hex_mrb_xc_set(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_context *cxt;
  cxt = (struct mrb_hexchat_context *)DATA_PTR(self);
  if (cxt->c != NULL) {
    hexchat_set_context(ph, cxt->c);
  }
  return mrb_nil_value();
}

// HexChat::Internal::Context#null?
static mrb_value
hex_mrb_xc_null(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_context *cxt;
  mrb_value result = mrb_false_value();
  cxt = (struct mrb_hexchat_context *)DATA_PTR(self);
  if (cxt->c == NULL) {
    result = mrb_true_value();
  }
  return result;
}

// HexChat::Internal::Context#ptr
static mrb_value
hex_mrb_xc_ptr(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_context *cxt;
  cxt = (struct mrb_hexchat_context *)DATA_PTR(self);
  return hex_mrb_ptr_to_s(mrb, (void *)cxt->c);
}

// HexChat::Internal::Hook#info
static mrb_value
hex_mrb_xh_info(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  return hex_mrb_hook_info(mrb, hk);
}

// HexChat::Internal::Hook#hooked?
static mrb_value
hex_mrb_xh_hooked(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  mrb_value result = mrb_false_value();
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  if (hk->xhook != NULL) {
    result = mrb_true_value();
  }
  return result;
}

// HexChat::Internal::Hook#unhook
static mrb_value
hex_mrb_xh_unhook(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  hex_mrb_hook_unhook(hk);
  return mrb_nil_value();
}

// HexChat::Internal::Hook#set_ref
static mrb_value
hex_mrb_xh_set_ref(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  mrb_value ref;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  mrb_get_args(mrb, "o", &ref);
  return hex_mrb_hook_set_ref(mrb, hk, ref);
}

// HexChat::Internal::Hook#get_ref
static mrb_value
hex_mrb_xh_get_ref(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  return hk->ref;
}

// Set $__HOOK_REF global, used to provide unhook call from within
// hook block.  Returns existing value.  Removes it completely if
// it is set to nil.
static mrb_value
mrb_set_gv_hook(mrb_state *mrb, mrb_value v)
{
  mrb_sym mid = mrb_intern_str(mrb, mrb_str_new_cstr(mrb, "$__HOOK_REF"));
  mrb_value gv_hook = mrb_gv_get(mrb, mid);
  if (mrb_obj_equal(mrb, v, mrb_nil_value())) {
    mrb_gv_remove(mrb, mid);
  } else {
    mrb_gv_set(mrb, mid, v);
  }
  return gv_hook;
}

// Command hook callback function
static int
hex_mrb_hook_command_cb(char *word[], char *word_eol[], struct mrb_hexchat_hook *hk)
{
  mrb_state *mrb = (mrb_state *)hk->mrb;
  mrb_value block = hk->block;
  mrb_value rb_word = hex_mrb_words_to_array(mrb, word, 1, 32);
  mrb_value rb_word_eol = hex_mrb_words_to_array(mrb, word_eol, 1, 32);
  mrb_value old_gv_hook = mrb_set_gv_hook(mrb, hk->ref);
  mrb_value result = mrb_funcall(mrb, block, "call", (mrb_int)2, rb_word, rb_word_eol);
  mrb_set_gv_hook(mrb, old_gv_hook);
  if (mrb->exc) {
    hexchat_print(ph, "error in command callback");
    hex_mrb_print_exc(mrb);
    mrb->exc = 0;
    return HEXCHAT_EAT_NONE;
  }
  return (int)mrb_fixnum(result);
}

// HexChat::Internal::Hook#hook_command(String, String, Integer)
static mrb_value
hex_mrb_xh_hook_command(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  char *cmd;
  char *help;
  int pri = HEXCHAT_PRI_NORM;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  mrb_get_args(mrb, "zz!|i", &cmd, &help, &pri);
  hex_mrb_hook_hook(mrb, hk, hexchat_hook_command(ph, cmd, pri, (void *)hex_mrb_hook_command_cb, help, (void *)hk));
  // printf("MRB command hooked %s\n", cmd);
  return mrb_nil_value();
}

// Print hook callback function
static int
hex_mrb_hook_print_cb(char *word[], struct mrb_hexchat_hook *hk)
{
  mrb_state *mrb = (mrb_state *)hk->mrb;
  mrb_value block = hk->block;
  mrb_value rb_word = hex_mrb_words_to_array(mrb, word, 1, 32);
  mrb_value old_gv_hook = mrb_set_gv_hook(mrb, hk->ref);
  mrb_value result = mrb_funcall(mrb, block, "call", (mrb_int)1, rb_word);
  mrb_set_gv_hook(mrb, old_gv_hook);
  if (mrb->exc) {
    hexchat_print(ph, "error in print callback");
    hex_mrb_print_exc(mrb);
    mrb->exc = 0;
    return HEXCHAT_EAT_NONE;
  }
  return (int)mrb_fixnum(result);
}

// HexChat::Internal::Hook#hook_print(String, Integer)
static mrb_value
hex_mrb_xh_hook_print(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  char *name;
  int pri = HEXCHAT_PRI_NORM;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  mrb_get_args(mrb, "z|i", &name, &pri);
  hex_mrb_hook_hook(mrb, hk, hexchat_hook_print(ph, name, pri, (void *)hex_mrb_hook_print_cb, (void *)hk));
  // printf("MRB print hooked %s\n", name);
  return mrb_nil_value();
}

// Server hook callback function
static int
hex_mrb_hook_server_cb(char *word[], char *word_eol[], struct mrb_hexchat_hook *hk)
{
  mrb_state *mrb = (mrb_state *)hk->mrb;
  mrb_value block = hk->block;
  mrb_value rb_word = hex_mrb_words_to_array(mrb, word, 1, 32);
  mrb_value rb_word_eol = hex_mrb_words_to_array(mrb, word_eol, 1, 32);
  mrb_value old_gv_hook = mrb_set_gv_hook(mrb, hk->ref);
  mrb_value result = mrb_funcall(mrb, block, "call", (mrb_int)2, rb_word, rb_word_eol);
  mrb_set_gv_hook(mrb, old_gv_hook);
  if (mrb->exc) {
    hexchat_print(ph, "error in server callback");
    hex_mrb_print_exc(mrb);
    mrb->exc = 0;
    return HEXCHAT_EAT_NONE;
  }
  return (int)mrb_fixnum(result);
}

// HexChat::Internal::Hook#hook_server(String, Integer)
static mrb_value
hex_mrb_xh_hook_server(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  char *name;
  int pri = HEXCHAT_PRI_NORM;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  mrb_get_args(mrb, "z|i", &name, &pri);
  hex_mrb_hook_hook(mrb, hk, hexchat_hook_server(ph, name, pri, (void *)hex_mrb_hook_server_cb, (void *)hk));
  // printf("MRB server hooked %s\n", name);
  return mrb_nil_value();
}

// Timer hook callback function
static int
hex_mrb_hook_timer_cb(struct mrb_hexchat_hook *hk)
{
  mrb_state *mrb = (mrb_state *)hk->mrb;
  mrb_value block = hk->block;
  mrb_value old_gv_hook = mrb_set_gv_hook(mrb, hk->ref);
  mrb_value result = mrb_funcall(mrb, block, "call", (mrb_int)0);
  mrb_set_gv_hook(mrb, old_gv_hook);
  if (mrb->exc) {
    hexchat_print(ph, "error in timer callback");
    hex_mrb_print_exc(mrb);
    mrb->exc = 0;
    return HEXCHAT_EAT_NONE;
  }
  return (int)mrb_fixnum(result);
}

// HexChat::Internal::Hook#hook_timer(Integer)
static mrb_value
hex_mrb_xh_hook_timer(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  int timeout = 0;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  mrb_get_args(mrb, "i", &timeout);
  hex_mrb_hook_hook(mrb, hk, hexchat_hook_timer(ph, timeout, (void *)hex_mrb_hook_timer_cb, (void *)hk));
  // printf("MRB hooked timer %d\n", timeout);
  return mrb_nil_value();
}

// FD hook callback function
static int
hex_hex_mrb_hook_fd_cb(int fd, int flags, struct mrb_hexchat_hook *hk)
{
  mrb_state *mrb = (mrb_state *)hk->mrb;
  mrb_value block = hk->block;
  mrb_value old_gv_hook = mrb_set_gv_hook(mrb, hk->ref);
  mrb_value result = mrb_funcall(mrb, block, "call", (mrb_int)2,
  mrb_fixnum_value((mrb_int)fd), mrb_fixnum_value((mrb_int)flags));
  mrb_set_gv_hook(mrb, old_gv_hook);
  if (mrb->exc) {
    hexchat_print(ph, "error in fd callback");
    hex_mrb_print_exc(mrb);
    mrb->exc = 0;
    return HEXCHAT_EAT_NONE;
  }
  return (int)mrb_fixnum(result);
}

// HexChat::Internal::Hook#hook_fd(Integer, Integer)
static mrb_value
hex_mrb_xh_hook_fd(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  int fd = 0;
  int flags = 0;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  mrb_get_args(mrb, "ii", &fd, &flags);
  hex_mrb_hook_hook(mrb, hk, hexchat_hook_fd(ph, fd, flags, (void *)hex_hex_mrb_hook_fd_cb, (void *)hk));
  // printf("MRB hooked fd %d\n", fd);
  return mrb_nil_value();
}

// HexChat::Internal::Hook.initialize(Block)
static mrb_value
hex_mrb_xh_initialize(mrb_state *mrb, mrb_value self)
{
  struct mrb_hexchat_hook *hk;
  mrb_value block;
  hk = (struct mrb_hexchat_hook *)DATA_PTR(self);
  if (hk) {
    hex_mrb_hook_free(mrb, hk);
  }
  mrb_data_init(self, NULL, &mrb_hexchat_hook_type);
  mrb_get_args(mrb, "&", &block);
  hk = hex_mrb_hook_alloc(mrb, block);
  mrb_data_init(self, hk, &mrb_hexchat_hook_type);
  return self;
}

// HexChat::Internal and HexChat::(constants) setup
static void
hex_mrb_internal_begin(mrb_state *mrb)
{
  mrbc_context *c = mrbc_context_new(mrb);
  hexchat_module = mrb_define_module(mrb, "HexChat");
  internal_class = mrb_define_class_under(mrb, hexchat_module, "Internal", mrb->object_class);
  cxt_class = mrb_define_class_under(mrb, internal_class, "Context", mrb->object_class);
  list_class = mrb_define_class_under(mrb, internal_class, "List", mrb->object_class);
  hook_class = mrb_define_class_under(mrb, internal_class, "Hook", mrb->object_class);
  // HexChat constants
  mrb_define_const(mrb, hexchat_module, "STRIP_COLOR", mrb_fixnum_value((mrb_int)1));
  mrb_define_const(mrb, hexchat_module, "STRIP_ATTR",  mrb_fixnum_value((mrb_int)2));
  mrb_define_const(mrb, hexchat_module, "STRIP_ALL",   mrb_fixnum_value((mrb_int)1|2));
  mrb_define_const(mrb, hexchat_module, "PRI_HIGHEST", mrb_fixnum_value((mrb_int)HEXCHAT_PRI_HIGHEST));
  mrb_define_const(mrb, hexchat_module, "PRI_HIGH",    mrb_fixnum_value((mrb_int)HEXCHAT_PRI_HIGH));
  mrb_define_const(mrb, hexchat_module, "PRI_NORM",    mrb_fixnum_value((mrb_int)HEXCHAT_PRI_NORM));
  mrb_define_const(mrb, hexchat_module, "PRI_LOW",     mrb_fixnum_value((mrb_int)HEXCHAT_PRI_LOW));
  mrb_define_const(mrb, hexchat_module, "PRI_LOWEST",  mrb_fixnum_value((mrb_int)HEXCHAT_PRI_LOWEST));
  mrb_define_const(mrb, hexchat_module, "FD_READ",     mrb_fixnum_value((mrb_int)HEXCHAT_FD_READ));
  mrb_define_const(mrb, hexchat_module, "FD_WRITE",    mrb_fixnum_value((mrb_int)HEXCHAT_FD_WRITE));
  mrb_define_const(mrb, hexchat_module, "FD_EXCEPTION", mrb_fixnum_value((mrb_int)HEXCHAT_FD_EXCEPTION));
  mrb_define_const(mrb, hexchat_module, "FD_NOTSOCKET", mrb_fixnum_value((mrb_int)HEXCHAT_FD_NOTSOCKET));
  mrb_define_const(mrb, hexchat_module, "EAT_NONE",    mrb_fixnum_value((mrb_int)HEXCHAT_EAT_NONE));
  mrb_define_const(mrb, hexchat_module, "EAT_HEXCHAT", mrb_fixnum_value((mrb_int)HEXCHAT_EAT_HEXCHAT));
  mrb_define_const(mrb, hexchat_module, "EAT_PLUGIN",  mrb_fixnum_value((mrb_int)HEXCHAT_EAT_PLUGIN));
  mrb_define_const(mrb, hexchat_module, "EAT_ALL",     mrb_fixnum_value((mrb_int)HEXCHAT_EAT_ALL));
  // HexChat::Internal methods
  mrb_define_class_method(mrb, internal_class, "print",     hex_mrb_xi_print, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, internal_class, "command",   hex_mrb_xi_command, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, internal_class, "get_info",  hex_mrb_xi_get_info, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, internal_class, "strip",     hex_mrb_xi_strip, MRB_ARGS_ARG(1,1));
  mrb_define_class_method(mrb, internal_class, "get_prefs", hex_mrb_xi_get_prefs, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, internal_class, "pluginpref_set_str", hex_mrb_xi_pluginpref_set_str, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, internal_class, "pluginpref_get_str", hex_mrb_xi_pluginpref_get_str, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, internal_class, "pluginpref_set_int", hex_mrb_xi_pluginpref_set_int, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, internal_class, "pluginpref_get_int", hex_mrb_xi_pluginpref_get_int, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, internal_class, "nickcmp",   hex_mrb_xi_nickcmp, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, internal_class, "emit_print", hex_mrb_xi_emit_print, MRB_ARGS_ARG(1,6));
  mrb_define_class_method(mrb, internal_class, "load",      hex_mrb_xi_load, MRB_ARGS_REQ(1));
  // HexChat::Internal::Context methods
  mrb_define_class_method(mrb, cxt_class, "current",  hex_mrb_xc_current, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, cxt_class, "find",     hex_mrb_xc_find, MRB_ARGS_OPT(2));
  mrb_define_method(mrb, cxt_class, "initialize", hex_mrb_xc_initialize, MRB_ARGS_OPT(2));
  mrb_define_method(mrb, cxt_class, "set",        hex_mrb_xc_set, MRB_ARGS_NONE());
  mrb_define_method(mrb, cxt_class, "null?",      hex_mrb_xc_null, MRB_ARGS_NONE());
  mrb_define_method(mrb, cxt_class, "ptr",        hex_mrb_xc_ptr, MRB_ARGS_NONE());
  // HexChat::Internal::List methods
  mrb_define_class_method(mrb, list_class, "get",     hex_mrb_xl_get, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, list_class, "fields",  hex_mrb_xl_fields, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, list_class, "next",    hex_mrb_xl_next, MRB_ARGS_NONE());
  mrb_define_method(mrb, list_class, "str",     hex_mrb_xl_str, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, list_class, "int",     hex_mrb_xl_int, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, list_class, "time",    hex_mrb_xl_time, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, list_class, "cxt",     hex_mrb_xl_cxt, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, list_class, "free",    hex_mrb_xl_free, MRB_ARGS_NONE());
  mrb_define_method(mrb, list_class, "free?",   hex_mrb_xl_free_q, MRB_ARGS_NONE());
  mrb_define_method(mrb, list_class, "initialize", hex_mrb_xl_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, list_class, "ptr", 	hex_mrb_xl_ptr, MRB_ARGS_NONE());
  // HexChat::Internal::Hook methods
  mrb_define_method(mrb, hook_class, "hooked?",       hex_mrb_xh_hooked, MRB_ARGS_NONE());
  mrb_define_method(mrb, hook_class, "info",          hex_mrb_xh_info, MRB_ARGS_NONE());
  mrb_define_method(mrb, hook_class, "set_ref",       hex_mrb_xh_set_ref, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, hook_class, "get_ref",       hex_mrb_xh_get_ref, MRB_ARGS_NONE());
  mrb_define_method(mrb, hook_class, "unhook",        hex_mrb_xh_unhook, MRB_ARGS_NONE());
  mrb_define_method(mrb, hook_class, "hook_command",  hex_mrb_xh_hook_command, MRB_ARGS_ARG(2,1));
  mrb_define_method(mrb, hook_class, "hook_fd",       hex_mrb_xh_hook_fd, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, hook_class, "hook_print",    hex_mrb_xh_hook_print, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, hook_class, "hook_server",   hex_mrb_xh_hook_server, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, hook_class, "hook_timer",    hex_mrb_xh_hook_timer, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, hook_class, "initialize",    hex_mrb_xh_initialize, MRB_ARGS_REQ(1));
  mrbc_filename(mrb, c, mrb_file_internal);
  c->lineno = 1;
  //mrb_load_string_cxt(mrb, xchat_rb, c);
  mrb_load_irep_cxt(mrb, hexchat_mrb_lib, c);
  mrbc_context_free(mrb, c);
  if (mrb->exc) {
    hexchat_print(ph, "error loading internal library:");
    hex_mrb_print_exc(mrb);
    mrb->exc = 0;
  }
  mrbc_filename(mrb, c, mrb_file_none);
}

// Clean up in preparation for shutdown
static void
hex_mrb_internal_end(mrb_state *mrb)
{
  struct RClass *hexchat_module = mrb_module_get(mrb, "HexChat");
  struct RClass *internal_class = mrb_class_get_under(mrb, hexchat_module, "Internal");
  mrb_value internal_class_v = mrb_obj_value(internal_class);
  mrb_sym mid = mrb_intern_str(mrb, mrb_str_new_cstr(mrb, "cleanup"));
  mrb_bool r = mrb_respond_to(mrb, internal_class_v, mid);
  if (r) {
    // printf("Calling HexChat::Internal#cleanup\n");
    mrb_funcall(mrb, internal_class_v, "cleanup", 0);
    if (mrb->exc) {
      hexchat_print(ph, "error calling HexChat::Internal#cleanup, possible leaks!");
      hex_mrb_print_exc(mrb);
      mrb->exc = 0;
    }
  } else {
    hexchat_print(ph, "Warning: HexChat::Internal#cleanup not defined, possible leaks!");
  }
  if (console_cxt != NULL) {
    mrbc_context_free(mrb, console_cxt);
  }
}

// Handle the /MRB command
// Just /MRB by itself opens the MRuby "console"
// /MRB EVAL <code> evals the code
// Attempt to pass everything else to HexChat::Internal.mrb_command
static int
hex_mrb_command_eval (char *word[], char *word_eol[], mrb_state *mrb)
{
  if (strlen(word_eol[2]) == 0) {
    if (console_cxt == NULL) {
            console_cxt = mrbc_context_new(mrb);
            console_cxt->lineno = 1;
    }
    hexchat_command(ph, "query >>MRuby<<");
  } else {
    char *cmd = word[2];
    if (strcasecmp(cmd, "EVAL") == 0) {
      mrb_value v;
      mrb_value inspect;
      mrbc_context *c = mrbc_context_new(mrb);
      c->lineno = 1;
      mrbc_filename(mrb, c, mrb_file_eval);
      v = mrb_load_string_cxt(mrb, word_eol[3], c);
      mrbc_context_free(mrb, c);
      if (mrb->exc) {
                      hexchat_print(ph, "MRuby: Error evaluating code:");
              hex_mrb_print_exc(mrb);
              mrb->exc = 0;
      }
      inspect = mrb_inspect(mrb, v);
      hexchat_printf(ph, "=> %s", mrb_str_to_cstr(mrb, inspect));
      mrbc_filename(mrb, c, mrb_file_none);
    } else {
      struct RClass *hexchat_module = mrb_module_get(mrb, "HexChat");
      struct RClass *internal_class = mrb_class_get_under(mrb, hexchat_module, "Internal");
      mrb_value internal_class_v = mrb_obj_value(internal_class);
      mrb_sym mid = mrb_intern_str(mrb, mrb_str_new_cstr(mrb, "mrb_command"));
      mrbc_context *c = mrbc_context_new(mrb);
      c->lineno = 1;
      mrbc_filename(mrb, c, mrb_file_internal);
      if (mrb_respond_to(mrb, internal_class_v, mid)) {
        mrb_value rb_word = hex_mrb_words_to_array(mrb, word, 1, 32);
        mrb_value rb_word_eol = hex_mrb_words_to_array(mrb, word_eol, 1, 32);
        mrb_funcall(mrb, internal_class_v, "mrb_command", (mrb_int)2, rb_word, rb_word_eol);
        if (mrb->exc) {
          hexchat_print(ph, "MRuby: Error executing command");
          hex_mrb_print_exc(mrb);
          mrb->exc = 0;
        }
      } else {
        hexchat_print(ph, "MRuby: HexChat::Internal#mrb_command not defined");
      }
      mrbc_context_free(mrb, c);
    }
  }
  return HEXCHAT_EAT_ALL;
}

// Handle stuff directed at the MRuby "console"
static int
mruby_console (char *word[], char *word_eol[], mrb_state *mrb)
{
  char *channel = (char *)hexchat_get_info(ph, "channel");
  if (channel && channel[0] == '>' && strcmp(channel, ">>MRuby<<") == 0) {
    mrb_value v;
    mrb_value inspect;
    hexchat_printf(ph, "[%d]> %s", console_cxt->lineno, word_eol[1]);
    mrbc_filename(mrb, console_cxt, mrb_file_console);
    v = mrb_load_string_cxt(mrb, word_eol[1], console_cxt);
    if (mrb->exc) {
      hex_mrb_print_exc(mrb);
      mrb->exc = 0;
      v = mrb_nil_value();
    }
    inspect = mrb_inspect(mrb, v);
    hexchat_printf(ph, "=> %s", mrb_str_to_cstr(mrb, inspect));
    console_cxt->lineno++;
    return HEXCHAT_EAT_ALL;
  }
  return HEXCHAT_EAT_NONE;
}

// HexChat interfacing - plugin info
void
hexchat_plugin_get_info (char **name, char **desc, char **version,
                                                          void **reserved)
{
  *name = "MRuby";
  *desc = "MRuby scripting interface";
  *version = MRUBY_VERSION;
  if (reserved)
          *reserved = NULL;
}

/* Reinit safeguard - borried from Perl plugin :-) */

static int initialized = 0;

// HexChat interfacing - plugin startup
int
hexchat_plugin_init (hexchat_plugin * plugin_handle, char **plugin_name,
                                                char **plugin_desc, char **plugin_version, char *arg)
{
  mrb_state *mrb;
  if (initialized != 0) {
          hexchat_print (plugin_handle, "MRuby plugin already loaded\n");
          return 0;
  }

  ph = plugin_handle;
  initialized = 1;

  *plugin_name = "MRuby";
  *plugin_desc = "MRuby scripting interface";
  *plugin_version = MRUBY_VERSION;

  hexchat_printf (ph, "MRuby %s plugin initializing", MRUBY_VERSION);
  // Initialize MRuby interpreter
  mrb = mrb_open();
  if (mrb == NULL) {
    hexchat_print (ph, "Invalid mrb_state");
    return HEXCHAT_EAT_HEXCHAT;
  }
  hex_g_mrb = mrb;
  mrb_gv_set(mrb, mrb_intern_lit(mrb, "$0"), mrb_str_new_cstr(mrb, "(HexChat)"));
  hex_mrb_internal_begin(mrb);

  hexchat_hook_command (ph, "mrb", HEXCHAT_PRI_NORM, (void *)hex_mrb_command_eval, "MRB [<command>] opens MRuby console or, if given, runs command (see MRB HELP)", (void *)mrb);
  hexchat_hook_command (ph, "", HEXCHAT_PRI_NORM, (void *)mruby_console, NULL, (void *)mrb);

  hexchat_printf (ph, "MRuby %s plugin loaded", MRUBY_VERSION);

  return 1;
}

// HexChat interfacing - Plugin shutdown
int
hexchat_plugin_deinit(hexchat_plugin *plugin_handle)
{
  hex_mrb_internal_end(hex_g_mrb);
  mrb_close(hex_g_mrb);

  initialized = 0;
  hexchat_printf(plugin_handle, "MRuby %s interface unloaded", MRUBY_VERSION);
  return 1;
}

