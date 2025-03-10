// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nvim/api/buffer.h"
#include "nvim/api/deprecated.h"
#include "nvim/api/private/converter.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/dispatch.h"
#include "nvim/api/private/helpers.h"
#include "nvim/api/vim.h"
#include "nvim/api/window.h"
#include "nvim/ascii.h"
#include "nvim/buffer.h"
#include "nvim/buffer_defs.h"
#include "nvim/charset.h"
#include "nvim/context.h"
#include "nvim/decoration.h"
#include "nvim/edit.h"
#include "nvim/eval.h"
#include "nvim/eval/typval.h"
#include "nvim/eval/userfunc.h"
#include "nvim/ex_cmds2.h"
#include "nvim/ex_docmd.h"
#include "nvim/file_search.h"
#include "nvim/fileio.h"
#include "nvim/getchar.h"
#include "nvim/globals.h"
#include "nvim/highlight.h"
#include "nvim/highlight_defs.h"
#include "nvim/lua/executor.h"
#include "nvim/mark.h"
#include "nvim/memline.h"
#include "nvim/memory.h"
#include "nvim/message.h"
#include "nvim/move.h"
#include "nvim/msgpack_rpc/channel.h"
#include "nvim/msgpack_rpc/helpers.h"
#include "nvim/ops.h"
#include "nvim/option.h"
#include "nvim/os/input.h"
#include "nvim/os/process.h"
#include "nvim/popupmnu.h"
#include "nvim/screen.h"
#include "nvim/state.h"
#include "nvim/syntax.h"
#include "nvim/types.h"
#include "nvim/ui.h"
#include "nvim/vim.h"
#include "nvim/viml/parser/expressions.h"
#include "nvim/viml/parser/parser.h"
#include "nvim/window.h"

#define LINE_BUFFER_SIZE 4096

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "api/vim.c.generated.h"
#endif

/// Gets a highlight definition by name.
///
/// @param name Highlight group name
/// @param rgb Export RGB colors
/// @param[out] err Error details, if any
/// @return Highlight definition map
/// @see nvim_get_hl_by_id
Dictionary nvim_get_hl_by_name(String name, Boolean rgb, Error *err)
  FUNC_API_SINCE(3)
{
  Dictionary result = ARRAY_DICT_INIT;
  int id = syn_name2id(name.data);

  if (id == 0) {
    api_set_error(err, kErrorTypeException, "Invalid highlight name: %s",
                  name.data);
    return result;
  }
  result = nvim_get_hl_by_id(id, rgb, err);
  return result;
}

/// Gets a highlight definition by id. |hlID()|
///
/// @param hl_id Highlight id as returned by |hlID()|
/// @param rgb Export RGB colors
/// @param[out] err Error details, if any
/// @return Highlight definition map
/// @see nvim_get_hl_by_name
Dictionary nvim_get_hl_by_id(Integer hl_id, Boolean rgb, Error *err)
  FUNC_API_SINCE(3)
{
  Dictionary dic = ARRAY_DICT_INIT;
  if (syn_get_final_id((int)hl_id) == 0) {
    api_set_error(err, kErrorTypeException,
                  "Invalid highlight id: %" PRId64, hl_id);
    return dic;
  }
  int attrcode = syn_id2attr((int)hl_id);
  return hl_get_attr_by_id(attrcode, rgb, err);
}

/// Gets a highlight group by name
///
/// similar to |hlID()|, but allocates a new ID if not present.
Integer nvim_get_hl_id_by_name(String name)
  FUNC_API_SINCE(7)
{
  return syn_check_group(name.data, (int)name.size);
}

Dictionary nvim__get_hl_defs(Integer ns_id, Error *err)
{
  if (ns_id == 0) {
    return get_global_hl_defs();
  }
  abort();
}

/// Set a highlight group.
///
/// @param ns_id number of namespace for this highlight. Use value 0
///              to set a highlight group in the global (`:highlight`)
///              namespace.
/// @param name highlight group name, like ErrorMsg
/// @param val highlight definition map, like |nvim_get_hl_by_name|.
///            in addition the following keys are also recognized:
///              `default`: don't override existing definition,
///                         like `hi default`
///              `ctermfg`: sets foreground of cterm color
///              `ctermbg`: sets background of cterm color
///              `cterm`  : cterm attribute map. sets attributed for
///                         cterm colors. similer to `hi cterm`
///                         Note: by default cterm attributes are
///                               same as attributes of gui color
/// @param[out] err Error details, if any
///
// TODO(bfredl): val should take update vs reset flag
void nvim_set_hl(Integer ns_id, String name, Dict(highlight) *val, Error *err)
  FUNC_API_SINCE(7)
{
  int hl_id = syn_check_group(name.data, (int)name.size);
  int link_id = -1;

  HlAttrs attrs = dict2hlattrs(val, true, &link_id, err);
  if (!ERROR_SET(err)) {
    ns_hl_def((NS)ns_id, hl_id, attrs, link_id, val);
  }
}

/// Set active namespace for highlights.
///
/// NB: this function can be called from async contexts, but the
/// semantics are not yet well-defined. To start with
/// |nvim_set_decoration_provider| on_win and on_line callbacks
/// are explicitly allowed to change the namespace during a redraw cycle.
///
/// @param ns_id the namespace to activate
/// @param[out] err Error details, if any
void nvim__set_hl_ns(Integer ns_id, Error *err)
  FUNC_API_FAST
{
  if (ns_id >= 0) {
    ns_hl_active = (NS)ns_id;
  }

  // TODO(bfredl): this is a little bit hackish.  Eventually we want a standard
  // event path for redraws caused by "fast" events. This could tie in with
  // better throttling of async events causing redraws, such as non-batched
  // nvim_buf_set_extmark calls from async contexts.
  if (!provider_active && !ns_hl_changed && must_redraw < NOT_VALID) {
    multiqueue_put(main_loop.events, on_redraw_event, 0);
  }
  ns_hl_changed = true;
}

static void on_redraw_event(void **argv)
{
  redraw_all_later(NOT_VALID);
}


/// Sends input-keys to Nvim, subject to various quirks controlled by `mode`
/// flags. This is a blocking call, unlike |nvim_input()|.
///
/// On execution error: does not fail, but updates v:errmsg.
///
/// To input sequences like <C-o> use |nvim_replace_termcodes()| (typically
/// with escape_ks=false) to replace |keycodes|, then pass the result to
/// nvim_feedkeys().
///
/// Example:
/// <pre>
///     :let key = nvim_replace_termcodes("<C-o>", v:true, v:false, v:true)
///     :call nvim_feedkeys(key, 'n', v:false)
/// </pre>
///
/// @param keys         to be typed
/// @param mode         behavior flags, see |feedkeys()|
/// @param escape_ks    If true, escape K_SPECIAL bytes in `keys`
///                     This should be false if you already used
///                     |nvim_replace_termcodes()|, and true otherwise.
/// @see feedkeys()
/// @see vim_strsave_escape_ks
void nvim_feedkeys(String keys, String mode, Boolean escape_ks)
  FUNC_API_SINCE(1)
{
  bool remap = true;
  bool insert = false;
  bool typed = false;
  bool execute = false;
  bool dangerous = false;

  for (size_t i = 0; i < mode.size; ++i) {
    switch (mode.data[i]) {
    case 'n':
      remap = false; break;
    case 'm':
      remap = true; break;
    case 't':
      typed = true; break;
    case 'i':
      insert = true; break;
    case 'x':
      execute = true; break;
    case '!':
      dangerous = true; break;
    }
  }

  if (keys.size == 0 && !execute) {
    return;
  }

  char *keys_esc;
  if (escape_ks) {
    // Need to escape K_SPECIAL before putting the string in the
    // typeahead buffer.
    keys_esc = (char *)vim_strsave_escape_ks((char_u *)keys.data);
  } else {
    keys_esc = keys.data;
  }
  ins_typebuf((char_u *)keys_esc, (remap ? REMAP_YES : REMAP_NONE),
              insert ? 0 : typebuf.tb_len, !typed, false);
  if (vgetc_busy) {
    typebuf_was_filled = true;
  }

  if (escape_ks) {
    xfree(keys_esc);
  }

  if (execute) {
    int save_msg_scroll = msg_scroll;

    // Avoid a 1 second delay when the keys start Insert mode.
    msg_scroll = false;
    if (!dangerous) {
      ex_normal_busy++;
    }
    exec_normal(true);
    if (!dangerous) {
      ex_normal_busy--;
    }
    msg_scroll |= save_msg_scroll;
  }
}

/// Queues raw user-input. Unlike |nvim_feedkeys()|, this uses a low-level
/// input buffer and the call is non-blocking (input is processed
/// asynchronously by the eventloop).
///
/// On execution error: does not fail, but updates v:errmsg.
///
/// @note |keycodes| like <CR> are translated, so "<" is special.
///       To input a literal "<", send <LT>.
///
/// @note For mouse events use |nvim_input_mouse()|. The pseudokey form
///       "<LeftMouse><col,row>" is deprecated since |api-level| 6.
///
/// @param keys to be typed
/// @return Number of bytes actually written (can be fewer than
///         requested if the buffer becomes full).
Integer nvim_input(String keys)
  FUNC_API_SINCE(1) FUNC_API_FAST
{
  return (Integer)input_enqueue(keys);
}

/// Send mouse event from GUI.
///
/// Non-blocking: does not wait on any result, but queues the event to be
/// processed soon by the event loop.
///
/// @note Currently this doesn't support "scripting" multiple mouse events
///       by calling it multiple times in a loop: the intermediate mouse
///       positions will be ignored. It should be used to implement real-time
///       mouse input in a GUI. The deprecated pseudokey form
///       ("<LeftMouse><col,row>") of |nvim_input()| has the same limitation.
///
/// @param button Mouse button: one of "left", "right", "middle", "wheel".
/// @param action For ordinary buttons, one of "press", "drag", "release".
///               For the wheel, one of "up", "down", "left", "right".
/// @param modifier String of modifiers each represented by a single char.
///                 The same specifiers are used as for a key press, except
///                 that the "-" separator is optional, so "C-A-", "c-a"
///                 and "CA" can all be used to specify Ctrl+Alt+click.
/// @param grid Grid number if the client uses |ui-multigrid|, else 0.
/// @param row Mouse row-position (zero-based, like redraw events)
/// @param col Mouse column-position (zero-based, like redraw events)
/// @param[out] err Error details, if any
void nvim_input_mouse(String button, String action, String modifier, Integer grid, Integer row,
                      Integer col, Error *err)
  FUNC_API_SINCE(6) FUNC_API_FAST
{
  if (button.data == NULL || action.data == NULL) {
    goto error;
  }

  int code = 0;

  if (strequal(button.data, "left")) {
    code = KE_LEFTMOUSE;
  } else if (strequal(button.data, "middle")) {
    code = KE_MIDDLEMOUSE;
  } else if (strequal(button.data, "right")) {
    code = KE_RIGHTMOUSE;
  } else if (strequal(button.data, "wheel")) {
    code = KE_MOUSEDOWN;
  } else {
    goto error;
  }

  if (code == KE_MOUSEDOWN) {
    if (strequal(action.data, "down")) {
      code = KE_MOUSEUP;
    } else if (strequal(action.data, "up")) {
      // code = KE_MOUSEDOWN
    } else if (strequal(action.data, "left")) {
      code = KE_MOUSERIGHT;
    } else if (strequal(action.data, "right")) {
      code = KE_MOUSELEFT;
    } else {
      goto error;
    }
  } else {
    if (strequal(action.data, "press")) {
      // pass
    } else if (strequal(action.data, "drag")) {
      code += KE_LEFTDRAG - KE_LEFTMOUSE;
    } else if (strequal(action.data, "release")) {
      code += KE_LEFTRELEASE - KE_LEFTMOUSE;
    } else {
      goto error;
    }
  }

  int modmask = 0;
  for (size_t i = 0; i < modifier.size; i++) {
    char byte = modifier.data[i];
    if (byte == '-') {
      continue;
    }
    int mod = name_to_mod_mask(byte);
    if (mod == 0) {
      api_set_error(err, kErrorTypeValidation,
                    "invalid modifier %c", byte);
      return;
    }
    modmask |= mod;
  }

  input_enqueue_mouse(code, (uint8_t)modmask, (int)grid, (int)row, (int)col);
  return;

error:
  api_set_error(err, kErrorTypeValidation,
                "invalid button or action");
}

/// Replaces terminal codes and |keycodes| (<CR>, <Esc>, ...) in a string with
/// the internal representation.
///
/// @param str        String to be converted.
/// @param from_part  Legacy Vim parameter. Usually true.
/// @param do_lt      Also translate <lt>. Ignored if `special` is false.
/// @param special    Replace |keycodes|, e.g. <CR> becomes a "\r" char.
/// @see replace_termcodes
/// @see cpoptions
String nvim_replace_termcodes(String str, Boolean from_part, Boolean do_lt, Boolean special)
  FUNC_API_SINCE(1)
{
  if (str.size == 0) {
    // Empty string
    return (String) { .data = NULL, .size = 0 };
  }

  char *ptr = NULL;
  replace_termcodes((char_u *)str.data, str.size, (char_u **)&ptr,
                    from_part, do_lt, special, CPO_TO_CPO_FLAGS);
  return cstr_as_string(ptr);
}


/// Execute Lua code. Parameters (if any) are available as `...` inside the
/// chunk. The chunk can return a value.
///
/// Only statements are executed. To evaluate an expression, prefix it
/// with `return`: return my_function(...)
///
/// @param code       Lua code to execute
/// @param args       Arguments to the code
/// @param[out] err   Details of an error encountered while parsing
///                   or executing the Lua code.
///
/// @return           Return value of Lua code if present or NIL.
Object nvim_exec_lua(String code, Array args, Error *err)
  FUNC_API_SINCE(7)
  FUNC_API_REMOTE_ONLY
{
  return nlua_exec(code, args, err);
}

/// Notify the user with a message
///
/// Relays the call to vim.notify . By default forwards your message in the
/// echo area but can be overridden to trigger desktop notifications.
///
/// @param msg        Message to display to the user
/// @param log_level  The log level
/// @param opts       Reserved for future use.
/// @param[out] err   Error details, if any
Object nvim_notify(String msg, Integer log_level, Dictionary opts, Error *err)
  FUNC_API_SINCE(7)
{
  FIXED_TEMP_ARRAY(args, 3);
  args.items[0] = STRING_OBJ(msg);
  args.items[1] = INTEGER_OBJ(log_level);
  args.items[2] = DICTIONARY_OBJ(opts);

  return nlua_exec(STATIC_CSTR_AS_STRING("return vim.notify(...)"), args, err);
}

/// Calculates the number of display cells occupied by `text`.
/// <Tab> counts as one cell.
///
/// @param text       Some text
/// @param[out] err   Error details, if any
/// @return Number of cells
Integer nvim_strwidth(String text, Error *err)
  FUNC_API_SINCE(1)
{
  if (text.size > INT_MAX) {
    api_set_error(err, kErrorTypeValidation, "String is too long");
    return 0;
  }

  return (Integer)mb_string2cells((char_u *)text.data);
}

/// Gets the paths contained in 'runtimepath'.
///
/// @return List of paths
ArrayOf(String) nvim_list_runtime_paths(Error *err)
  FUNC_API_SINCE(1)
{
  return nvim_get_runtime_file(NULL_STRING, true, err);
}

Array nvim__runtime_inspect(void)
{
  return runtime_inspect();
}

/// Find files in runtime directories
///
/// 'name' can contain wildcards. For example
/// nvim_get_runtime_file("colors/*.vim", true) will return all color
/// scheme files. Always use forward slashes (/) in the search pattern for
/// subdirectories regardless of platform.
///
/// It is not an error to not find any files. An empty array is returned then.
///
/// @param name pattern of files to search for
/// @param all whether to return all matches or only the first
/// @return list of absolute paths to the found files
ArrayOf(String) nvim_get_runtime_file(String name, Boolean all, Error *err)
  FUNC_API_SINCE(7)
  FUNC_API_FAST
{
  Array rv = ARRAY_DICT_INIT;

  int flags = DIP_DIRFILE | (all ? DIP_ALL : 0);

  do_in_runtimepath((char_u *)(name.size ? name.data : ""),
                    flags, find_runtime_cb, &rv);
  return rv;
}

static void find_runtime_cb(char_u *fname, void *cookie)
{
  Array *rv = (Array *)cookie;
  if (fname != NULL) {
    ADD(*rv, STRING_OBJ(cstr_to_string((char *)fname)));
  }
}

String nvim__get_lib_dir(void)
{
  return cstr_as_string(get_lib_dir());
}

/// Find files in runtime directories
///
/// @param pat pattern of files to search for
/// @param all whether to return all matches or only the first
/// @param options
///          is_lua: only search lua subdirs
/// @return list of absolute paths to the found files
ArrayOf(String) nvim__get_runtime(Array pat, Boolean all, Dict(runtime) *opts, Error *err)
  FUNC_API_SINCE(8)
  FUNC_API_FAST
{
  bool is_lua = api_object_to_bool(opts->is_lua, "is_lua", false, err);
  if (ERROR_SET(err)) {
    return (Array)ARRAY_DICT_INIT;
  }
  return runtime_get_named(is_lua, pat, all);
}


/// Changes the global working directory.
///
/// @param dir      Directory path
/// @param[out] err Error details, if any
void nvim_set_current_dir(String dir, Error *err)
  FUNC_API_SINCE(1)
{
  if (dir.size >= MAXPATHL) {
    api_set_error(err, kErrorTypeValidation, "Directory name is too long");
    return;
  }

  char_u string[MAXPATHL];
  memcpy(string, dir.data, dir.size);
  string[dir.size] = NUL;

  try_start();

  if (!changedir_func(string, kCdScopeGlobal)) {
    if (!try_end(err)) {
      api_set_error(err, kErrorTypeException, "Failed to change directory");
    }
    return;
  }

  try_end(err);
}

/// Gets the current line.
///
/// @param[out] err Error details, if any
/// @return Current line string
String nvim_get_current_line(Error *err)
  FUNC_API_SINCE(1)
{
  return buffer_get_line(curbuf->handle, curwin->w_cursor.lnum - 1, err);
}

/// Sets the current line.
///
/// @param line     Line contents
/// @param[out] err Error details, if any
void nvim_set_current_line(String line, Error *err)
  FUNC_API_SINCE(1)
  FUNC_API_CHECK_TEXTLOCK
{
  buffer_set_line(curbuf->handle, curwin->w_cursor.lnum - 1, line, err);
}

/// Deletes the current line.
///
/// @param[out] err Error details, if any
void nvim_del_current_line(Error *err)
  FUNC_API_SINCE(1)
  FUNC_API_CHECK_TEXTLOCK
{
  buffer_del_line(curbuf->handle, curwin->w_cursor.lnum - 1, err);
}

/// Gets a global (g:) variable.
///
/// @param name     Variable name
/// @param[out] err Error details, if any
/// @return Variable value
Object nvim_get_var(String name, Error *err)
  FUNC_API_SINCE(1)
{
  return dict_get_value(&globvardict, name, err);
}

/// Sets a global (g:) variable.
///
/// @param name     Variable name
/// @param value    Variable value
/// @param[out] err Error details, if any
void nvim_set_var(String name, Object value, Error *err)
  FUNC_API_SINCE(1)
{
  dict_set_var(&globvardict, name, value, false, false, err);
}

/// Removes a global (g:) variable.
///
/// @param name     Variable name
/// @param[out] err Error details, if any
void nvim_del_var(String name, Error *err)
  FUNC_API_SINCE(1)
{
  dict_set_var(&globvardict, name, NIL, true, false, err);
}

/// Gets a v: variable.
///
/// @param name     Variable name
/// @param[out] err Error details, if any
/// @return         Variable value
Object nvim_get_vvar(String name, Error *err)
  FUNC_API_SINCE(1)
{
  return dict_get_value(&vimvardict, name, err);
}

/// Sets a v: variable, if it is not readonly.
///
/// @param name     Variable name
/// @param value    Variable value
/// @param[out] err Error details, if any
void nvim_set_vvar(String name, Object value, Error *err)
  FUNC_API_SINCE(6)
{
  dict_set_var(&vimvardict, name, value, false, false, err);
}

/// Gets the global value of an option.
///
/// @param name     Option name
/// @param[out] err Error details, if any
/// @return         Option value (global)
Object nvim_get_option(String name, Error *err)
  FUNC_API_SINCE(1)
{
  return get_option_from(NULL, SREQ_GLOBAL, name, err);
}

/// Gets the value of an option. The behavior of this function matches that of
/// |:set|: the local value of an option is returned if it exists; otherwise,
/// the global value is returned. Local values always correspond to the current
/// buffer or window. To get a buffer-local or window-local option for a
/// specific buffer or window, use |nvim_buf_get_option()| or
/// |nvim_win_get_option()|.
///
/// @param name      Option name
/// @param opts      Optional parameters
///                  - scope: One of 'global' or 'local'. Analogous to
///                  |:setglobal| and |:setlocal|, respectively.
/// @param[out] err  Error details, if any
/// @return          Option value
Object nvim_get_option_value(String name, Dict(option) *opts, Error *err)
  FUNC_API_SINCE(9)
{
  Object rv = OBJECT_INIT;

  int scope = 0;
  if (opts->scope.type == kObjectTypeString) {
    if (!strcmp(opts->scope.data.string.data, "local")) {
      scope = OPT_LOCAL;
    } else if (!strcmp(opts->scope.data.string.data, "global")) {
      scope = OPT_GLOBAL;
    } else {
      api_set_error(err, kErrorTypeValidation, "invalid scope: must be 'local' or 'global'");
      goto end;
    }
  } else if (HAS_KEY(opts->scope)) {
    api_set_error(err, kErrorTypeValidation, "invalid value for key: scope");
    goto end;
  }

  long numval = 0;
  char *stringval = NULL;
  switch (get_option_value(name.data, &numval, (char_u **)&stringval, scope)) {
  case 0:
    rv = STRING_OBJ(cstr_as_string(stringval));
    break;
  case 1:
    rv = INTEGER_OBJ(numval);
    break;
  case 2:
    switch (numval) {
      case 0:
      case 1:
        rv = BOOLEAN_OBJ(numval);
        break;
      default:
        // Boolean options that return something other than 0 or 1 should return nil. Currently this
        // only applies to 'autoread' which uses -1 as a local value to indicate "unset"
        rv = NIL;
        break;
    }
    break;
  default:
    api_set_error(err, kErrorTypeValidation, "unknown option '%s'", name.data);
    goto end;
  }

end:
  return rv;
}

/// Sets the value of an option. The behavior of this function matches that of
/// |:set|: for global-local options, both the global and local value are set
/// unless otherwise specified with {scope}.
///
/// @param name      Option name
/// @param value     New option value
/// @param opts      Optional parameters
///                  - scope: One of 'global' or 'local'. Analogous to
///                  |:setglobal| and |:setlocal|, respectively.
/// @param[out] err  Error details, if any
void nvim_set_option_value(String name, Object value, Dict(option) *opts, Error *err)
  FUNC_API_SINCE(9)
{
  int scope = 0;
  if (opts->scope.type == kObjectTypeString) {
    if (!strcmp(opts->scope.data.string.data, "local")) {
      scope = OPT_LOCAL;
    } else if (!strcmp(opts->scope.data.string.data, "global")) {
      scope = OPT_GLOBAL;
    } else {
      api_set_error(err, kErrorTypeValidation, "invalid scope: must be 'local' or 'global'");
      return;
    }
  } else if (HAS_KEY(opts->scope)) {
    api_set_error(err, kErrorTypeValidation, "invalid value for key: scope");
    return;
  }

  long numval = 0;
  char *stringval = NULL;

  switch (value.type) {
  case kObjectTypeInteger:
    numval = value.data.integer;
    break;
  case kObjectTypeBoolean:
    numval = value.data.boolean ? 1 : 0;
    break;
  case kObjectTypeString:
    stringval = value.data.string.data;
    break;
  case kObjectTypeNil:
    scope |= OPT_CLEAR;
    break;
  default:
    api_set_error(err, kErrorTypeValidation, "invalid value for option");
    return;
  }

  char *e = set_option_value(name.data, numval, stringval, scope);
  if (e) {
    api_set_error(err, kErrorTypeException, "%s", e);
  }
}

/// Gets the option information for all options.
///
/// The dictionary has the full option names as keys and option metadata
/// dictionaries as detailed at |nvim_get_option_info|.
///
/// @return dictionary of all options
Dictionary nvim_get_all_options_info(Error *err)
  FUNC_API_SINCE(7)
{
  return get_all_vimoptions();
}

/// Gets the option information for one option
///
/// Resulting dictionary has keys:
///     - name: Name of the option (like 'filetype')
///     - shortname: Shortened name of the option (like 'ft')
///     - type: type of option ("string", "number" or "boolean")
///     - default: The default value for the option
///     - was_set: Whether the option was set.
///
///     - last_set_sid: Last set script id (if any)
///     - last_set_linenr: line number where option was set
///     - last_set_chan: Channel where option was set (0 for local)
///
///     - scope: one of "global", "win", or "buf"
///     - global_local: whether win or buf option has a global value
///
///     - commalist: List of comma separated values
///     - flaglist: List of single char flags
///
///
/// @param          name Option name
/// @param[out] err Error details, if any
/// @return         Option Information
Dictionary nvim_get_option_info(String name, Error *err)
  FUNC_API_SINCE(7)
{
  return get_vimoption(name, err);
}

/// Sets the global value of an option.
///
/// @param channel_id
/// @param name     Option name
/// @param value    New option value
/// @param[out] err Error details, if any
void nvim_set_option(uint64_t channel_id, String name, Object value, Error *err)
  FUNC_API_SINCE(1)
{
  set_option_to(channel_id, NULL, SREQ_GLOBAL, name, value, err);
}

/// Echo a message.
///
/// @param chunks  A list of [text, hl_group] arrays, each representing a
///                text chunk with specified highlight. `hl_group` element
///                can be omitted for no highlight.
/// @param history  if true, add to |message-history|.
/// @param opts  Optional parameters. Reserved for future use.
void nvim_echo(Array chunks, Boolean history, Dictionary opts, Error *err)
  FUNC_API_SINCE(7)
{
  HlMessage hl_msg = parse_hl_msg(chunks, err);
  if (ERROR_SET(err)) {
    goto error;
  }

  if (opts.size > 0) {
    api_set_error(err, kErrorTypeValidation, "opts dict isn't empty");
    goto error;
  }

  no_wait_return++;
  msg_start();
  msg_clr_eos();
  bool need_clear = false;
  for (uint32_t i = 0; i < kv_size(hl_msg); i++) {
    HlMessageChunk chunk = kv_A(hl_msg, i);
    msg_multiline_attr((const char *)chunk.text.data, chunk.attr,
                       true, &need_clear);
  }
  if (history) {
    msg_ext_set_kind("echomsg");
    add_hl_msg_hist(hl_msg);
  } else {
    msg_ext_set_kind("echo");
  }
  no_wait_return--;
  msg_end();

error:
  clear_hl_msg(&hl_msg);
}

/// Writes a message to the Vim output buffer. Does not append "\n", the
/// message is buffered (won't display) until a linefeed is written.
///
/// @param str Message
void nvim_out_write(String str)
  FUNC_API_SINCE(1)
{
  write_msg(str, false);
}

/// Writes a message to the Vim error buffer. Does not append "\n", the
/// message is buffered (won't display) until a linefeed is written.
///
/// @param str Message
void nvim_err_write(String str)
  FUNC_API_SINCE(1)
{
  write_msg(str, true);
}

/// Writes a message to the Vim error buffer. Appends "\n", so the buffer is
/// flushed (and displayed).
///
/// @param str Message
/// @see nvim_err_write()
void nvim_err_writeln(String str)
  FUNC_API_SINCE(1)
{
  nvim_err_write(str);
  nvim_err_write((String) { .data = "\n", .size = 1 });
}

/// Gets the current list of buffer handles
///
/// Includes unlisted (unloaded/deleted) buffers, like `:ls!`.
/// Use |nvim_buf_is_loaded()| to check if a buffer is loaded.
///
/// @return List of buffer handles
ArrayOf(Buffer) nvim_list_bufs(void)
  FUNC_API_SINCE(1)
{
  Array rv = ARRAY_DICT_INIT;

  FOR_ALL_BUFFERS(b) {
    rv.size++;
  }

  rv.items = xmalloc(sizeof(Object) * rv.size);
  size_t i = 0;

  FOR_ALL_BUFFERS(b) {
    rv.items[i++] = BUFFER_OBJ(b->handle);
  }

  return rv;
}

/// Gets the current buffer.
///
/// @return Buffer handle
Buffer nvim_get_current_buf(void)
  FUNC_API_SINCE(1)
{
  return curbuf->handle;
}

/// Sets the current buffer.
///
/// @param buffer   Buffer handle
/// @param[out] err Error details, if any
void nvim_set_current_buf(Buffer buffer, Error *err)
  FUNC_API_SINCE(1)
  FUNC_API_CHECK_TEXTLOCK
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return;
  }

  try_start();
  int result = do_buffer(DOBUF_GOTO, DOBUF_FIRST, FORWARD, buf->b_fnum, 0);
  if (!try_end(err) && result == FAIL) {
    api_set_error(err,
                  kErrorTypeException,
                  "Failed to switch to buffer %d",
                  buffer);
  }
}

/// Gets the current list of window handles.
///
/// @return List of window handles
ArrayOf(Window) nvim_list_wins(void)
  FUNC_API_SINCE(1)
{
  Array rv = ARRAY_DICT_INIT;

  FOR_ALL_TAB_WINDOWS(tp, wp) {
    rv.size++;
  }

  rv.items = xmalloc(sizeof(Object) * rv.size);
  size_t i = 0;

  FOR_ALL_TAB_WINDOWS(tp, wp) {
    rv.items[i++] = WINDOW_OBJ(wp->handle);
  }

  return rv;
}

/// Gets the current window.
///
/// @return Window handle
Window nvim_get_current_win(void)
  FUNC_API_SINCE(1)
{
  return curwin->handle;
}

/// Sets the current window.
///
/// @param window Window handle
/// @param[out] err Error details, if any
void nvim_set_current_win(Window window, Error *err)
  FUNC_API_SINCE(1)
  FUNC_API_CHECK_TEXTLOCK
{
  win_T *win = find_window_by_handle(window, err);

  if (!win) {
    return;
  }

  try_start();
  goto_tabpage_win(win_find_tabpage(win), win);
  if (!try_end(err) && win != curwin) {
    api_set_error(err,
                  kErrorTypeException,
                  "Failed to switch to window %d",
                  window);
  }
}

/// Creates a new, empty, unnamed buffer.
///
/// @param listed Sets 'buflisted'
/// @param scratch Creates a "throwaway" |scratch-buffer| for temporary work
///                (always 'nomodified'). Also sets 'nomodeline' on the buffer.
/// @param[out] err Error details, if any
/// @return Buffer handle, or 0 on error
///
/// @see buf_open_scratch
Buffer nvim_create_buf(Boolean listed, Boolean scratch, Error *err)
  FUNC_API_SINCE(6)
{
  try_start();
  buf_T *buf = buflist_new(NULL, NULL, (linenr_T)0,
                           BLN_NOOPT | BLN_NEW | (listed ? BLN_LISTED : 0));
  try_end(err);
  if (buf == NULL) {
    goto fail;
  }

  // Open the memline for the buffer. This will avoid spurious autocmds when
  // a later nvim_buf_set_lines call would have needed to "open" the buffer.
  try_start();
  block_autocmds();
  int status = ml_open(buf);
  unblock_autocmds();
  try_end(err);
  if (status == FAIL) {
    goto fail;
  }

  if (scratch) {
    aco_save_T aco;
    aucmd_prepbuf(&aco, buf);
    set_option_value("bufhidden", 0L, "hide", OPT_LOCAL);
    set_option_value("buftype", 0L, "nofile", OPT_LOCAL);
    set_option_value("swapfile", 0L, NULL, OPT_LOCAL);
    set_option_value("modeline", 0L, NULL, OPT_LOCAL);  // 'nomodeline'
    aucmd_restbuf(&aco);
  }
  return buf->b_fnum;

fail:
  if (!ERROR_SET(err)) {
    api_set_error(err, kErrorTypeException, "Failed to create buffer");
  }
  return 0;
}

/// Open a terminal instance in a buffer
///
/// By default (and currently the only option) the terminal will not be
/// connected to an external process. Instead, input send on the channel
/// will be echoed directly by the terminal. This is useful to display
/// ANSI terminal sequences returned as part of a rpc message, or similar.
///
/// Note: to directly initiate the terminal using the right size, display the
/// buffer in a configured window before calling this. For instance, for a
/// floating display, first create an empty buffer using |nvim_create_buf()|,
/// then display it using |nvim_open_win()|, and then  call this function.
/// Then |nvim_chan_send()| can be called immediately to process sequences
/// in a virtual terminal having the intended size.
///
/// @param buffer the buffer to use (expected to be empty)
/// @param opts   Optional parameters.
///          - on_input: lua callback for input sent, i e keypresses in terminal
///            mode. Note: keypresses are sent raw as they would be to the pty
///            master end. For instance, a carriage return is sent
///            as a "\r", not as a "\n". |textlock| applies. It is possible
///            to call |nvim_chan_send| directly in the callback however.
///                 ["input", term, bufnr, data]
/// @param[out] err Error details, if any
/// @return Channel id, or 0 on error
Integer nvim_open_term(Buffer buffer, DictionaryOf(LuaRef) opts, Error *err)
  FUNC_API_SINCE(7)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (!buf) {
    return 0;
  }

  LuaRef cb = LUA_NOREF;
  for (size_t i = 0; i < opts.size; i++) {
    String k = opts.items[i].key;
    Object *v = &opts.items[i].value;
    if (strequal("on_input", k.data)) {
      if (v->type != kObjectTypeLuaRef) {
        api_set_error(err, kErrorTypeValidation,
                      "%s is not a function", "on_input");
        return 0;
      }
      cb = v->data.luaref;
      v->data.luaref = LUA_NOREF;
      break;
    } else {
      api_set_error(err, kErrorTypeValidation, "unexpected key: %s", k.data);
    }
  }

  TerminalOptions topts;
  Channel *chan = channel_alloc(kChannelStreamInternal);
  chan->stream.internal.cb = cb;
  topts.data = chan;
  // NB: overridden in terminal_check_size if a window is already
  // displaying the buffer
  topts.width = (uint16_t)MAX(curwin->w_width_inner - win_col_off(curwin), 0);
  topts.height = (uint16_t)curwin->w_height_inner;
  topts.write_cb = term_write;
  topts.resize_cb = term_resize;
  topts.close_cb = term_close;
  Terminal *term = terminal_open(buf, topts);
  terminal_check_size(term);
  chan->term = term;
  return (Integer)chan->id;
}

static void term_write(char *buf, size_t size, void *data)
{
  Channel *chan = data;
  LuaRef cb = chan->stream.internal.cb;
  if (cb == LUA_NOREF) {
    return;
  }
  FIXED_TEMP_ARRAY(args, 3);
  args.items[0] = INTEGER_OBJ((Integer)chan->id);
  args.items[1] = BUFFER_OBJ(terminal_buf(chan->term));
  args.items[2] = STRING_OBJ(((String){ .data = buf, .size = size }));
  textlock++;
  nlua_call_ref(cb, "input", args, false, NULL);
  textlock--;
}

static void term_resize(uint16_t width, uint16_t height, void *data)
{
  // TODO(bfredl): lua callback
}

static void term_close(void *data)
{
  Channel *chan = data;
  terminal_destroy(chan->term);
  chan->term = NULL;
  api_free_luaref(chan->stream.internal.cb);
  chan->stream.internal.cb = LUA_NOREF;
  channel_decref(chan);
}


/// Send data to channel `id`. For a job, it writes it to the
/// stdin of the process. For the stdio channel |channel-stdio|,
/// it writes to Nvim's stdout.  For an internal terminal instance
/// (|nvim_open_term()|) it writes directly to terminal output.
/// See |channel-bytes| for more information.
///
/// This function writes raw data, not RPC messages.  If the channel
/// was created with `rpc=true` then the channel expects RPC
/// messages, use |vim.rpcnotify()| and |vim.rpcrequest()| instead.
///
/// @param chan id of the channel
/// @param data data to write. 8-bit clean: can contain NUL bytes.
/// @param[out] err Error details, if any
void nvim_chan_send(Integer chan, String data, Error *err)
  FUNC_API_SINCE(7) FUNC_API_REMOTE_ONLY FUNC_API_LUA_ONLY
{
  const char *error = NULL;
  if (!data.size) {
    return;
  }

  channel_send((uint64_t)chan, data.data, data.size,
               false, &error);
  if (error) {
    api_set_error(err, kErrorTypeValidation, "%s", error);
  }
}

/// Gets the current list of tabpage handles.
///
/// @return List of tabpage handles
ArrayOf(Tabpage) nvim_list_tabpages(void)
  FUNC_API_SINCE(1)
{
  Array rv = ARRAY_DICT_INIT;

  FOR_ALL_TABS(tp) {
    rv.size++;
  }

  rv.items = xmalloc(sizeof(Object) * rv.size);
  size_t i = 0;

  FOR_ALL_TABS(tp) {
    rv.items[i++] = TABPAGE_OBJ(tp->handle);
  }

  return rv;
}

/// Gets the current tabpage.
///
/// @return Tabpage handle
Tabpage nvim_get_current_tabpage(void)
  FUNC_API_SINCE(1)
{
  return curtab->handle;
}

/// Sets the current tabpage.
///
/// @param tabpage  Tabpage handle
/// @param[out] err Error details, if any
void nvim_set_current_tabpage(Tabpage tabpage, Error *err)
  FUNC_API_SINCE(1)
  FUNC_API_CHECK_TEXTLOCK
{
  tabpage_T *tp = find_tab_by_handle(tabpage, err);

  if (!tp) {
    return;
  }

  try_start();
  goto_tabpage_tp(tp, true, true);
  if (!try_end(err) && tp != curtab) {
    api_set_error(err,
                  kErrorTypeException,
                  "Failed to switch to tabpage %d",
                  tabpage);
  }
}

/// Pastes at cursor, in any mode.
///
/// Invokes the `vim.paste` handler, which handles each mode appropriately.
/// Sets redo/undo. Faster than |nvim_input()|. Lines break at LF ("\n").
///
/// Errors ('nomodifiable', `vim.paste()` failure, …) are reflected in `err`
/// but do not affect the return value (which is strictly decided by
/// `vim.paste()`).  On error, subsequent calls are ignored ("drained") until
/// the next paste is initiated (phase 1 or -1).
///
/// @param data  Multiline input. May be binary (containing NUL bytes).
/// @param crlf  Also break lines at CR and CRLF.
/// @param phase  -1: paste in a single call (i.e. without streaming).
///               To "stream" a paste, call `nvim_paste` sequentially with
///               these `phase` values:
///                 - 1: starts the paste (exactly once)
///                 - 2: continues the paste (zero or more times)
///                 - 3: ends the paste (exactly once)
/// @param[out] err Error details, if any
/// @return
///     - true: Client may continue pasting.
///     - false: Client must cancel the paste.
Boolean nvim_paste(String data, Boolean crlf, Integer phase, Error *err)
  FUNC_API_SINCE(6)
  FUNC_API_CHECK_TEXTLOCK
{
  static bool draining = false;
  bool cancel = false;

  if (phase < -1 || phase > 3) {
    api_set_error(err, kErrorTypeValidation, "Invalid phase: %" PRId64, phase);
    return false;
  }
  Array args = ARRAY_DICT_INIT;
  Object rv = OBJECT_INIT;
  if (phase == -1 || phase == 1) {  // Start of paste-stream.
    draining = false;
  } else if (draining) {
    // Skip remaining chunks.  Report error only once per "stream".
    goto theend;
  }
  Array lines = string_to_array(data, crlf);
  ADD(args, ARRAY_OBJ(lines));
  ADD(args, INTEGER_OBJ(phase));
  rv = nvim_exec_lua(STATIC_CSTR_AS_STRING("return vim.paste(...)"), args,
                     err);
  if (ERROR_SET(err)) {
    draining = true;
    goto theend;
  }
  if (!(State & (CMDLINE | INSERT)) && (phase == -1 || phase == 1)) {
    ResetRedobuff();
    AppendCharToRedobuff('a');  // Dot-repeat.
  }
  // vim.paste() decides if client should cancel.  Errors do NOT cancel: we
  // want to drain remaining chunks (rather than divert them to main input).
  cancel = (rv.type == kObjectTypeBoolean && !rv.data.boolean);
  if (!cancel && !(State & CMDLINE)) {  // Dot-repeat.
    for (size_t i = 0; i < lines.size; i++) {
      String s = lines.items[i].data.string;
      assert(data.size <= INT_MAX);
      AppendToRedobuffLit((char_u *)s.data, (int)s.size);
      // readfile()-style: "\n" is indicated by presence of N+1 item.
      if (i + 1 < lines.size) {
        AppendCharToRedobuff(NL);
      }
    }
  }
  if (!(State & (CMDLINE | INSERT)) && (phase == -1 || phase == 3)) {
    AppendCharToRedobuff(ESC);  // Dot-repeat.
  }
theend:
  api_free_object(rv);
  api_free_array(args);
  if (cancel || phase == -1 || phase == 3) {  // End of paste-stream.
    draining = false;
  }

  return !cancel;
}

/// Puts text at cursor, in any mode.
///
/// Compare |:put| and |p| which are always linewise.
///
/// @param lines  |readfile()|-style list of lines. |channel-lines|
/// @param type  Edit behavior: any |getregtype()| result, or:
///              - "b" |blockwise-visual| mode (may include width, e.g. "b3")
///              - "c" |charwise| mode
///              - "l" |linewise| mode
///              - ""  guess by contents, see |setreg()|
/// @param after  If true insert after cursor (like |p|), or before (like |P|).
/// @param follow  If true place cursor at end of inserted text.
/// @param[out] err Error details, if any
void nvim_put(ArrayOf(String) lines, String type, Boolean after, Boolean follow, Error *err)
  FUNC_API_SINCE(6)
  FUNC_API_CHECK_TEXTLOCK
{
  yankreg_T *reg = xcalloc(1, sizeof(yankreg_T));
  if (!prepare_yankreg_from_object(reg, type, lines.size)) {
    api_set_error(err, kErrorTypeValidation, "Invalid type: '%s'", type.data);
    goto cleanup;
  }
  if (lines.size == 0) {
    goto cleanup;  // Nothing to do.
  }

  for (size_t i = 0; i < lines.size; i++) {
    if (lines.items[i].type != kObjectTypeString) {
      api_set_error(err, kErrorTypeValidation,
                    "Invalid lines (expected array of strings)");
      goto cleanup;
    }
    String line = lines.items[i].data.string;
    reg->y_array[i] = (char_u *)xmemdupz(line.data, line.size);
    memchrsub(reg->y_array[i], NUL, NL, line.size);
  }

  finish_yankreg_from_object(reg, false);

  TRY_WRAP({
    try_start();
    bool VIsual_was_active = VIsual_active;
    msg_silent++;  // Avoid "N more lines" message.
    do_put(0, reg, after ? FORWARD : BACKWARD, 1, follow ? PUT_CURSEND : 0);
    msg_silent--;
    VIsual_active = VIsual_was_active;
    try_end(err);
  });

cleanup:
  free_register(reg);
  xfree(reg);
}

/// Subscribes to event broadcasts.
///
/// @param channel_id Channel id (passed automatically by the dispatcher)
/// @param event      Event type string
void nvim_subscribe(uint64_t channel_id, String event)
  FUNC_API_SINCE(1) FUNC_API_REMOTE_ONLY
{
  size_t length = (event.size < METHOD_MAXLEN ? event.size : METHOD_MAXLEN);
  char e[METHOD_MAXLEN + 1];
  memcpy(e, event.data, length);
  e[length] = NUL;
  rpc_subscribe(channel_id, e);
}

/// Unsubscribes to event broadcasts.
///
/// @param channel_id Channel id (passed automatically by the dispatcher)
/// @param event      Event type string
void nvim_unsubscribe(uint64_t channel_id, String event)
  FUNC_API_SINCE(1) FUNC_API_REMOTE_ONLY
{
  size_t length = (event.size < METHOD_MAXLEN ?
                   event.size :
                   METHOD_MAXLEN);
  char e[METHOD_MAXLEN + 1];
  memcpy(e, event.data, length);
  e[length] = NUL;
  rpc_unsubscribe(channel_id, e);
}

/// Returns the 24-bit RGB value of a |nvim_get_color_map()| color name or
/// "#rrggbb" hexadecimal string.
///
/// Example:
/// <pre>
///     :echo nvim_get_color_by_name("Pink")
///     :echo nvim_get_color_by_name("#cbcbcb")
/// </pre>
///
/// @param name Color name or "#rrggbb" string
/// @return 24-bit RGB value, or -1 for invalid argument.
Integer nvim_get_color_by_name(String name)
  FUNC_API_SINCE(1)
{
  return name_to_color(name.data);
}

/// Returns a map of color names and RGB values.
///
/// Keys are color names (e.g. "Aqua") and values are 24-bit RGB color values
/// (e.g. 65535).
///
/// @return Map of color names and RGB values.
Dictionary nvim_get_color_map(void)
  FUNC_API_SINCE(1)
{
  Dictionary colors = ARRAY_DICT_INIT;

  for (int i = 0; color_name_table[i].name != NULL; i++) {
    PUT(colors, color_name_table[i].name,
        INTEGER_OBJ(color_name_table[i].color));
  }
  return colors;
}

/// Gets a map of the current editor state.
///
/// @param opts  Optional parameters.
///               - types:  List of |context-types| ("regs", "jumps", "bufs",
///                 "gvars", …) to gather, or empty for "all".
/// @param[out]  err  Error details, if any
///
/// @return map of global |context|.
Dictionary nvim_get_context(Dict(context) *opts, Error *err)
  FUNC_API_SINCE(6)
{
  Array types = ARRAY_DICT_INIT;
  if (opts->types.type == kObjectTypeArray) {
    types = opts->types.data.array;
  } else if (opts->types.type != kObjectTypeNil) {
    api_set_error(err, kErrorTypeValidation, "invalid value for key: types");
    return (Dictionary)ARRAY_DICT_INIT;
  }

  int int_types = types.size > 0 ? 0 : kCtxAll;
  if (types.size > 0) {
    for (size_t i = 0; i < types.size; i++) {
      if (types.items[i].type == kObjectTypeString) {
        const char *const s = types.items[i].data.string.data;
        if (strequal(s, "regs")) {
          int_types |= kCtxRegs;
        } else if (strequal(s, "jumps")) {
          int_types |= kCtxJumps;
        } else if (strequal(s, "bufs")) {
          int_types |= kCtxBufs;
        } else if (strequal(s, "gvars")) {
          int_types |= kCtxGVars;
        } else if (strequal(s, "sfuncs")) {
          int_types |= kCtxSFuncs;
        } else if (strequal(s, "funcs")) {
          int_types |= kCtxFuncs;
        } else {
          api_set_error(err, kErrorTypeValidation, "unexpected type: %s", s);
          return (Dictionary)ARRAY_DICT_INIT;
        }
      }
    }
  }

  Context ctx = CONTEXT_INIT;
  ctx_save(&ctx, int_types);
  Dictionary dict = ctx_to_dict(&ctx);
  ctx_free(&ctx);
  return dict;
}

/// Sets the current editor state from the given |context| map.
///
/// @param  dict  |Context| map.
Object nvim_load_context(Dictionary dict)
  FUNC_API_SINCE(6)
{
  Context ctx = CONTEXT_INIT;

  int save_did_emsg = did_emsg;
  did_emsg = false;

  ctx_from_dict(dict, &ctx);
  if (!did_emsg) {
    ctx_restore(&ctx, kCtxAll);
  }

  ctx_free(&ctx);

  did_emsg = save_did_emsg;
  return (Object)OBJECT_INIT;
}

/// Gets the current mode. |mode()|
/// "blocking" is true if Nvim is waiting for input.
///
/// @returns Dictionary { "mode": String, "blocking": Boolean }
Dictionary nvim_get_mode(void)
  FUNC_API_SINCE(2) FUNC_API_FAST
{
  Dictionary rv = ARRAY_DICT_INIT;
  char *modestr = get_mode();
  bool blocked = input_blocking();

  PUT(rv, "mode", STRING_OBJ(cstr_as_string(modestr)));
  PUT(rv, "blocking", BOOLEAN_OBJ(blocked));

  return rv;
}

/// Gets a list of global (non-buffer-local) |mapping| definitions.
///
/// @param  mode       Mode short-name ("n", "i", "v", ...)
/// @returns Array of maparg()-like dictionaries describing mappings.
///          The "buffer" key is always zero.
ArrayOf(Dictionary) nvim_get_keymap(uint64_t channel_id, String mode)
  FUNC_API_SINCE(3)
{
  return keymap_array(mode, NULL, channel_id == LUA_INTERNAL_CALL);
}

/// Sets a global |mapping| for the given mode.
///
/// To set a buffer-local mapping, use |nvim_buf_set_keymap()|.
///
/// Unlike |:map|, leading/trailing whitespace is accepted as part of the {lhs}
/// or {rhs}. Empty {rhs} is |<Nop>|. |keycodes| are replaced as usual.
///
/// Example:
/// <pre>
///     call nvim_set_keymap('n', ' <NL>', '', {'nowait': v:true})
/// </pre>
///
/// is equivalent to:
/// <pre>
///     nmap <nowait> <Space><NL> <Nop>
/// </pre>
///
/// @param  mode  Mode short-name (map command prefix: "n", "i", "v", "x", …)
///               or "!" for |:map!|, or empty string for |:map|.
/// @param  lhs   Left-hand-side |{lhs}| of the mapping.
/// @param  rhs   Right-hand-side |{rhs}| of the mapping.
/// @param  opts  Optional parameters map. Accepts all |:map-arguments|
///               as keys excluding |<buffer>| but including |noremap| and "desc".
///               |desc| can be used to give a description to keymap.
///               When called from Lua, also accepts a "callback" key that takes
///               a Lua function to call when the mapping is executed.
///               Values are Booleans. Unknown key is an error.
/// @param[out]   err   Error details, if any.
void nvim_set_keymap(String mode, String lhs, String rhs, Dict(keymap) *opts, Error *err)
  FUNC_API_SINCE(6)
{
  modify_keymap(-1, false, mode, lhs, rhs, opts, err);
}

/// Unmaps a global |mapping| for the given mode.
///
/// To unmap a buffer-local mapping, use |nvim_buf_del_keymap()|.
///
/// @see |nvim_set_keymap()|
void nvim_del_keymap(String mode, String lhs, Error *err)
  FUNC_API_SINCE(6)
{
  nvim_buf_del_keymap(-1, mode, lhs, err);
}

/// Gets a map of global (non-buffer-local) Ex commands.
///
/// Currently only |user-commands| are supported, not builtin Ex commands.
///
/// @param  opts  Optional parameters. Currently only supports
///               {"builtin":false}
/// @param[out]  err   Error details, if any.
///
/// @returns Map of maps describing commands.
Dictionary nvim_get_commands(Dict(get_commands) *opts, Error *err)
  FUNC_API_SINCE(4)
{
  return nvim_buf_get_commands(-1, opts, err);
}

/// Returns a 2-tuple (Array), where item 0 is the current channel id and item
/// 1 is the |api-metadata| map (Dictionary).
///
/// @returns 2-tuple [{channel-id}, {api-metadata}]
Array nvim_get_api_info(uint64_t channel_id)
  FUNC_API_SINCE(1) FUNC_API_FAST FUNC_API_REMOTE_ONLY
{
  Array rv = ARRAY_DICT_INIT;

  assert(channel_id <= INT64_MAX);
  ADD(rv, INTEGER_OBJ((int64_t)channel_id));
  ADD(rv, DICTIONARY_OBJ(api_metadata()));

  return rv;
}

/// Self-identifies the client.
///
/// The client/plugin/application should call this after connecting, to provide
/// hints about its identity and purpose, for debugging and orchestration.
///
/// Can be called more than once; the caller should merge old info if
/// appropriate. Example: library first identifies the channel, then a plugin
/// using that library later identifies itself.
///
/// @note "Something is better than nothing". You don't need to include all the
///       fields.
///
/// @param channel_id
/// @param name Short name for the connected client
/// @param version  Dictionary describing the version, with these
///                 (optional) keys:
///     - "major" major version (defaults to 0 if not set, for no release yet)
///     - "minor" minor version
///     - "patch" patch number
///     - "prerelease" string describing a prerelease, like "dev" or "beta1"
///     - "commit" hash or similar identifier of commit
/// @param type Must be one of the following values. Client libraries should
///             default to "remote" unless overridden by the user.
///     - "remote" remote client connected to Nvim.
///     - "ui" gui frontend
///     - "embedder" application using Nvim as a component (for example,
///                  IDE/editor implementing a vim mode).
///     - "host" plugin host, typically started by nvim
///     - "plugin" single plugin, started by nvim
/// @param methods Builtin methods in the client. For a host, this does not
///                include plugin methods which will be discovered later.
///                The key should be the method name, the values are dicts with
///                these (optional) keys (more keys may be added in future
///                versions of Nvim, thus unknown keys are ignored. Clients
///                must only use keys defined in this or later versions of
///                Nvim):
///     - "async"  if true, send as a notification. If false or unspecified,
///                use a blocking request
///     - "nargs" Number of arguments. Could be a single integer or an array
///                of two integers, minimum and maximum inclusive.
///
/// @param attributes Arbitrary string:string map of informal client properties.
///     Suggested keys:
///     - "website": Client homepage URL (e.g. GitHub repository)
///     - "license": License description ("Apache 2", "GPLv3", "MIT", …)
///     - "logo":    URI or path to image, preferably small logo or icon.
///                  .png or .svg format is preferred.
///
/// @param[out] err Error details, if any
void nvim_set_client_info(uint64_t channel_id, String name, Dictionary version, String type,
                          Dictionary methods, Dictionary attributes, Error *err)
  FUNC_API_SINCE(4) FUNC_API_REMOTE_ONLY
{
  Dictionary info = ARRAY_DICT_INIT;
  PUT(info, "name", copy_object(STRING_OBJ(name)));

  version = copy_dictionary(version);
  bool has_major = false;
  for (size_t i = 0; i < version.size; i++) {
    if (strequal(version.items[i].key.data, "major")) {
      has_major = true;
      break;
    }
  }
  if (!has_major) {
    PUT(version, "major", INTEGER_OBJ(0));
  }
  PUT(info, "version", DICTIONARY_OBJ(version));

  PUT(info, "type", copy_object(STRING_OBJ(type)));
  PUT(info, "methods", DICTIONARY_OBJ(copy_dictionary(methods)));
  PUT(info, "attributes", DICTIONARY_OBJ(copy_dictionary(attributes)));

  rpc_set_client_info(channel_id, info);
}

/// Gets information about a channel.
///
/// @returns Dictionary describing a channel, with these keys:
///    - "id"       Channel id.
///    - "argv"     (optional) Job arguments list.
///    - "stream"   Stream underlying the channel.
///         - "stdio"      stdin and stdout of this Nvim instance
///         - "stderr"     stderr of this Nvim instance
///         - "socket"     TCP/IP socket or named pipe
///         - "job"        Job with communication over its stdio.
///    -  "mode"    How data received on the channel is interpreted.
///         - "bytes"      Send and receive raw bytes.
///         - "terminal"   |terminal| instance interprets ASCII sequences.
///         - "rpc"        |RPC| communication on the channel is active.
///    -  "pty"     (optional) Name of pseudoterminal. On a POSIX system this
///                 is a device path like "/dev/pts/1". If the name is unknown,
///                 the key will still be present if a pty is used (e.g. for
///                 winpty on Windows).
///    -  "buffer"  (optional) Buffer with connected |terminal| instance.
///    -  "client"  (optional) Info about the peer (client on the other end of
///                 the RPC channel), if provided by it via
///                 |nvim_set_client_info()|.
///
Dictionary nvim_get_chan_info(Integer chan, Error *err)
  FUNC_API_SINCE(4)
{
  if (chan < 0) {
    return (Dictionary)ARRAY_DICT_INIT;
  }
  return channel_info((uint64_t)chan);
}

/// Get information about all open channels.
///
/// @returns Array of Dictionaries, each describing a channel with
///          the format specified at |nvim_get_chan_info()|.
Array nvim_list_chans(void)
  FUNC_API_SINCE(4)
{
  return channel_all_info();
}

/// Calls many API methods atomically.
///
/// This has two main usages:
/// 1. To perform several requests from an async context atomically, i.e.
///    without interleaving redraws, RPC requests from other clients, or user
///    interactions (however API methods may trigger autocommands or event
///    processing which have such side effects, e.g. |:sleep| may wake timers).
/// 2. To minimize RPC overhead (roundtrips) of a sequence of many requests.
///
/// @param channel_id
/// @param calls an array of calls, where each call is described by an array
///              with two elements: the request name, and an array of arguments.
/// @param[out] err Validation error details (malformed `calls` parameter),
///             if any. Errors from batched calls are given in the return value.
///
/// @return Array of two elements. The first is an array of return
/// values. The second is NIL if all calls succeeded. If a call resulted in
/// an error, it is a three-element array with the zero-based index of the call
/// which resulted in an error, the error type and the error message. If an
/// error occurred, the values from all preceding calls will still be returned.
Array nvim_call_atomic(uint64_t channel_id, Array calls, Error *err)
  FUNC_API_SINCE(1) FUNC_API_REMOTE_ONLY
{
  Array rv = ARRAY_DICT_INIT;
  Array results = ARRAY_DICT_INIT;
  Error nested_error = ERROR_INIT;

  size_t i;  // also used for freeing the variables
  for (i = 0; i < calls.size; i++) {
    if (calls.items[i].type != kObjectTypeArray) {
      api_set_error(err,
                    kErrorTypeValidation,
                    "Items in calls array must be arrays");
      goto validation_error;
    }
    Array call = calls.items[i].data.array;
    if (call.size != 2) {
      api_set_error(err,
                    kErrorTypeValidation,
                    "Items in calls array must be arrays of size 2");
      goto validation_error;
    }

    if (call.items[0].type != kObjectTypeString) {
      api_set_error(err,
                    kErrorTypeValidation,
                    "Name must be String");
      goto validation_error;
    }
    String name = call.items[0].data.string;

    if (call.items[1].type != kObjectTypeArray) {
      api_set_error(err,
                    kErrorTypeValidation,
                    "Args must be Array");
      goto validation_error;
    }
    Array args = call.items[1].data.array;

    MsgpackRpcRequestHandler handler =
      msgpack_rpc_get_handler_for(name.data,
                                  name.size,
                                  &nested_error);

    if (ERROR_SET(&nested_error)) {
      break;
    }
    Object result = handler.fn(channel_id, args, &nested_error);
    if (ERROR_SET(&nested_error)) {
      // error handled after loop
      break;
    }

    ADD(results, result);
  }

  ADD(rv, ARRAY_OBJ(results));
  if (ERROR_SET(&nested_error)) {
    Array errval = ARRAY_DICT_INIT;
    ADD(errval, INTEGER_OBJ((Integer)i));
    ADD(errval, INTEGER_OBJ(nested_error.type));
    ADD(errval, STRING_OBJ(cstr_to_string(nested_error.msg)));
    ADD(rv, ARRAY_OBJ(errval));
  } else {
    ADD(rv, NIL);
  }
  goto theend;

validation_error:
  api_free_array(results);
theend:
  api_clear_error(&nested_error);
  return rv;
}

/// Writes a message to vim output or error buffer. The string is split
/// and flushed after each newline. Incomplete lines are kept for writing
/// later.
///
/// @param message  Message to write
/// @param to_err   true: message is an error (uses `emsg` instead of `msg`)
static void write_msg(String message, bool to_err)
{
  static size_t out_pos = 0, err_pos = 0;
  static char out_line_buf[LINE_BUFFER_SIZE], err_line_buf[LINE_BUFFER_SIZE];

#define PUSH_CHAR(i, pos, line_buf, msg) \
  if (message.data[i] == NL || pos == LINE_BUFFER_SIZE - 1) { \
    line_buf[pos] = NUL; \
    msg(line_buf); \
    pos = 0; \
    continue; \
  } \
  line_buf[pos++] = message.data[i];

  ++no_wait_return;
  for (uint32_t i = 0; i < message.size; i++) {
    if (got_int) {
      break;
    }
    if (to_err) {
      PUSH_CHAR(i, err_pos, err_line_buf, emsg);
    } else {
      PUSH_CHAR(i, out_pos, out_line_buf, msg);
    }
  }
  --no_wait_return;
  msg_end();
}

// Functions used for testing purposes

/// Returns object given as argument.
///
/// This API function is used for testing. One should not rely on its presence
/// in plugins.
///
/// @param[in]  obj  Object to return.
///
/// @return its argument.
Object nvim__id(Object obj)
{
  return copy_object(obj);
}

/// Returns array given as argument.
///
/// This API function is used for testing. One should not rely on its presence
/// in plugins.
///
/// @param[in]  arr  Array to return.
///
/// @return its argument.
Array nvim__id_array(Array arr)
{
  return copy_object(ARRAY_OBJ(arr)).data.array;
}

/// Returns dictionary given as argument.
///
/// This API function is used for testing. One should not rely on its presence
/// in plugins.
///
/// @param[in]  dct  Dictionary to return.
///
/// @return its argument.
Dictionary nvim__id_dictionary(Dictionary dct)
{
  return copy_object(DICTIONARY_OBJ(dct)).data.dictionary;
}

/// Returns floating-point value given as argument.
///
/// This API function is used for testing. One should not rely on its presence
/// in plugins.
///
/// @param[in]  flt  Value to return.
///
/// @return its argument.
Float nvim__id_float(Float flt)
{
  return flt;
}

/// Gets internal stats.
///
/// @return Map of various internal stats.
Dictionary nvim__stats(void)
{
  Dictionary rv = ARRAY_DICT_INIT;
  PUT(rv, "fsync", INTEGER_OBJ(g_stats.fsync));
  PUT(rv, "redraw", INTEGER_OBJ(g_stats.redraw));
  PUT(rv, "lua_refcount", INTEGER_OBJ(nlua_refcount));
  return rv;
}

/// Gets a list of dictionaries representing attached UIs.
///
/// @return Array of UI dictionaries, each with these keys:
///   - "height"  Requested height of the UI
///   - "width"   Requested width of the UI
///   - "rgb"     true if the UI uses RGB colors (false implies |cterm-colors|)
///   - "ext_..." Requested UI extensions, see |ui-option|
///   - "chan"    Channel id of remote UI (not present for TUI)
Array nvim_list_uis(void)
  FUNC_API_SINCE(4)
{
  return ui_array();
}

/// Gets the immediate children of process `pid`.
///
/// @return Array of child process ids, empty if process not found.
Array nvim_get_proc_children(Integer pid, Error *err)
  FUNC_API_SINCE(4)
{
  Array rvobj = ARRAY_DICT_INIT;
  int *proc_list = NULL;

  if (pid <= 0 || pid > INT_MAX) {
    api_set_error(err, kErrorTypeException, "Invalid pid: %" PRId64, pid);
    goto end;
  }

  size_t proc_count;
  int rv = os_proc_children((int)pid, &proc_list, &proc_count);
  if (rv != 0) {
    // syscall failed (possibly because of kernel options), try shelling out.
    DLOG("fallback to vim._os_proc_children()");
    Array a = ARRAY_DICT_INIT;
    ADD(a, INTEGER_OBJ(pid));
    String s = cstr_to_string("return vim._os_proc_children(select(1, ...))");
    Object o = nlua_exec(s, a, err);
    api_free_string(s);
    api_free_array(a);
    if (o.type == kObjectTypeArray) {
      rvobj = o.data.array;
    } else if (!ERROR_SET(err)) {
      api_set_error(err, kErrorTypeException,
                    "Failed to get process children. pid=%" PRId64 " error=%d",
                    pid, rv);
    }
    goto end;
  }

  for (size_t i = 0; i < proc_count; i++) {
    ADD(rvobj, INTEGER_OBJ(proc_list[i]));
  }

end:
  xfree(proc_list);
  return rvobj;
}

/// Gets info describing process `pid`.
///
/// @return Map of process properties, or NIL if process not found.
Object nvim_get_proc(Integer pid, Error *err)
  FUNC_API_SINCE(4)
{
  Object rvobj = OBJECT_INIT;
  rvobj.data.dictionary = (Dictionary)ARRAY_DICT_INIT;
  rvobj.type = kObjectTypeDictionary;

  if (pid <= 0 || pid > INT_MAX) {
    api_set_error(err, kErrorTypeException, "Invalid pid: %" PRId64, pid);
    return NIL;
  }
#ifdef WIN32
  rvobj.data.dictionary = os_proc_info((int)pid);
  if (rvobj.data.dictionary.size == 0) {  // Process not found.
    return NIL;
  }
#else
  // Cross-platform process info APIs are miserable, so use `ps` instead.
  Array a = ARRAY_DICT_INIT;
  ADD(a, INTEGER_OBJ(pid));
  String s = cstr_to_string("return vim._os_proc_info(select(1, ...))");
  Object o = nlua_exec(s, a, err);
  api_free_string(s);
  api_free_array(a);
  if (o.type == kObjectTypeArray && o.data.array.size == 0) {
    return NIL;  // Process not found.
  } else if (o.type == kObjectTypeDictionary) {
    rvobj.data.dictionary = o.data.dictionary;
  } else if (!ERROR_SET(err)) {
    api_set_error(err, kErrorTypeException,
                  "Failed to get process info. pid=%" PRId64, pid);
  }
#endif
  return rvobj;
}

/// Selects an item in the completion popupmenu.
///
/// If |ins-completion| is not active this API call is silently ignored.
/// Useful for an external UI using |ui-popupmenu| to control the popupmenu
/// with the mouse. Can also be used in a mapping; use <cmd> |:map-cmd| to
/// ensure the mapping doesn't end completion mode.
///
/// @param item   Index (zero-based) of the item to select. Value of -1 selects
///               nothing and restores the original text.
/// @param insert Whether the selection should be inserted in the buffer.
/// @param finish Finish the completion and dismiss the popupmenu. Implies
///               `insert`.
/// @param  opts  Optional parameters. Reserved for future use.
/// @param[out] err Error details, if any
void nvim_select_popupmenu_item(Integer item, Boolean insert, Boolean finish, Dictionary opts,
                                Error *err)
  FUNC_API_SINCE(6)
{
  if (opts.size > 0) {
    api_set_error(err, kErrorTypeValidation, "opts dict isn't empty");
    return;
  }

  if (finish) {
    insert = true;
  }

  pum_ext_select_item((int)item, insert, finish);
}

/// NB: if your UI doesn't use hlstate, this will not return hlstate first time
Array nvim__inspect_cell(Integer grid, Integer row, Integer col, Error *err)
{
  Array ret = ARRAY_DICT_INIT;

  // TODO(bfredl): if grid == 0 we should read from the compositor's buffer.
  // The only problem is that it does not yet exist.
  ScreenGrid *g = &default_grid;
  if (grid == pum_grid.handle) {
    g = &pum_grid;
  } else if (grid > 1) {
    win_T *wp = get_win_by_grid_handle((handle_T)grid);
    if (wp != NULL && wp->w_grid_alloc.chars != NULL) {
      g = &wp->w_grid_alloc;
    } else {
      api_set_error(err, kErrorTypeValidation,
                    "No grid with the given handle");
      return ret;
    }
  }

  if (row < 0 || row >= g->Rows
      || col < 0 || col >= g->Columns) {
    return ret;
  }
  size_t off = g->line_offset[(size_t)row] + (size_t)col;
  ADD(ret, STRING_OBJ(cstr_to_string((char *)g->chars[off])));
  int attr = g->attrs[off];
  ADD(ret, DICTIONARY_OBJ(hl_get_attr_by_id(attr, true, err)));
  // will not work first time
  if (!highlight_use_hlstate()) {
    ADD(ret, ARRAY_OBJ(hl_inspect(attr)));
  }
  return ret;
}

void nvim__screenshot(String path)
  FUNC_API_FAST
{
  ui_call_screenshot(path);
}


/// Deletes an uppercase/file named mark. See |mark-motions|.
///
/// @note fails with error if a lowercase or buffer local named mark is used.
/// @param name       Mark name
/// @return true if the mark was deleted, else false.
/// @see |nvim_buf_del_mark()|
/// @see |nvim_get_mark()|
Boolean nvim_del_mark(String name, Error *err)
  FUNC_API_SINCE(8)
{
  bool res = false;
  if (name.size != 1) {
    api_set_error(err, kErrorTypeValidation,
                  "Mark name must be a single character");
    return res;
  }
  // Only allow file/uppercase marks
  // TODO(muniter): Refactor this ASCII_ISUPPER macro to a proper function
  if (ASCII_ISUPPER(*name.data) || ascii_isdigit(*name.data)) {
    res = set_mark(NULL, name, 0, 0, err);
  } else {
    api_set_error(err, kErrorTypeValidation,
                  "Only file/uppercase marks allowed, invalid mark name: '%c'",
                  *name.data);
  }
  return res;
}

/// Return a tuple (row, col, buffer, buffername) representing the position of
/// the uppercase/file named mark. See |mark-motions|.
///
/// Marks are (1,0)-indexed. |api-indexing|
///
/// @note fails with error if a lowercase or buffer local named mark is used.
/// @param name       Mark name
/// @param opts       Optional parameters. Reserved for future use.
/// @return 4-tuple (row, col, buffer, buffername), (0, 0, 0, '') if the mark is
/// not set.
/// @see |nvim_buf_set_mark()|
/// @see |nvim_del_mark()|
Array nvim_get_mark(String name, Dictionary opts, Error *err)
  FUNC_API_SINCE(8)
{
  Array rv = ARRAY_DICT_INIT;

  if (name.size != 1) {
    api_set_error(err, kErrorTypeValidation,
                  "Mark name must be a single character");
    return rv;
  } else if (!(ASCII_ISUPPER(*name.data) || ascii_isdigit(*name.data))) {
    api_set_error(err, kErrorTypeValidation,
                  "Only file/uppercase marks allowed, invalid mark name: '%c'",
                  *name.data);
    return rv;
  }

  xfmark_T mark = get_global_mark(*name.data);
  pos_T pos = mark.fmark.mark;
  bool allocated = false;
  int bufnr;
  char *filename;

  // Marks are from an open buffer it fnum is non zero
  if (mark.fmark.fnum != 0) {
    bufnr = mark.fmark.fnum;
    filename = (char *)buflist_nr2name(bufnr, true, true);
    allocated = true;
    // Marks comes from shada
  } else {
    filename = (char *)mark.fname;
    bufnr = 0;
  }

  bool exists = filename != NULL;
  Integer row;
  Integer col;

  if (!exists || pos.lnum <= 0) {
    if (allocated) {
      xfree(filename);
      allocated = false;
    }
    filename = "";
    bufnr = 0;
    row = 0;
    col = 0;
  } else {
    row = pos.lnum;
    col = pos.col;
  }

  ADD(rv, INTEGER_OBJ(row));
  ADD(rv, INTEGER_OBJ(col));
  ADD(rv, INTEGER_OBJ(bufnr));
  ADD(rv, STRING_OBJ(cstr_to_string(filename)));

  if (allocated) {
    xfree(filename);
  }

  return rv;
}

/// Evaluates statusline string.
///
/// @param str Statusline string (see 'statusline').
/// @param opts Optional parameters.
///           - winid: (number) |window-ID| of the window to use as context for statusline.
///           - maxwidth: (number) Maximum width of statusline.
///           - fillchar: (string) Character to fill blank spaces in the statusline (see
///                                'fillchars').
///           - highlights: (boolean) Return highlight information.
///           - use_tabline: (boolean) Evaluate tabline instead of statusline. When |TRUE|, {winid}
///                                    is ignored.
///
/// @param[out] err Error details, if any.
/// @return Dictionary containing statusline information, with these keys:
///       - str: (string) Characters that will be displayed on the statusline.
///       - width: (number) Display width of the statusline.
///       - highlights: Array containing highlight information of the statusline. Only included when
///                     the "highlights" key in {opts} is |TRUE|. Each element of the array is a
///                     |Dictionary| with these keys:
///           - start: (number) Byte index (0-based) of first character that uses the highlight.
///           - group: (string) Name of highlight group.
Dictionary nvim_eval_statusline(String str, Dict(eval_statusline) *opts, Error *err)
  FUNC_API_SINCE(8) FUNC_API_FAST
{
  Dictionary result = ARRAY_DICT_INIT;

  int maxwidth;
  int fillchar = 0;
  Window window = 0;
  bool use_tabline = false;
  bool highlights = false;

  if (HAS_KEY(opts->winid)) {
    if (opts->winid.type != kObjectTypeInteger) {
      api_set_error(err, kErrorTypeValidation, "winid must be an integer");
      return result;
    }

    window = (Window)opts->winid.data.integer;
  }

  if (HAS_KEY(opts->fillchar)) {
    if (opts->fillchar.type != kObjectTypeString || opts->fillchar.data.string.size == 0
        || char2cells(fillchar = utf_ptr2char((char_u *)opts->fillchar.data.string.data)) != 1
        || (size_t)utf_char2len(fillchar) != opts->fillchar.data.string.size) {
      api_set_error(err, kErrorTypeValidation, "fillchar must be a single-width character");
      return result;
    }
  }

  if (HAS_KEY(opts->highlights)) {
    highlights = api_object_to_bool(opts->highlights, "highlights", false, err);

    if (ERROR_SET(err)) {
      return result;
    }
  }

  if (HAS_KEY(opts->use_tabline)) {
    use_tabline = api_object_to_bool(opts->use_tabline, "use_tabline", false, err);

    if (ERROR_SET(err)) {
      return result;
    }
  }

  win_T *wp, *ewp;

  if (use_tabline) {
    wp = NULL;
    ewp = curwin;
    fillchar = ' ';
  } else {
    wp = find_window_by_handle(window, err);

    if (wp == NULL) {
      api_set_error(err, kErrorTypeException, "unknown winid %d", window);
      return result;
    }
    ewp = wp;

    if (fillchar == 0) {
      int attr;
      fillchar = fillchar_status(&attr, wp);
    }
  }

  if (HAS_KEY(opts->maxwidth)) {
    if (opts->maxwidth.type != kObjectTypeInteger) {
      api_set_error(err, kErrorTypeValidation, "maxwidth must be an integer");
      return result;
    }

    maxwidth = (int)opts->maxwidth.data.integer;
  } else {
    maxwidth = use_tabline ? Columns : wp->w_width;
  }

  char buf[MAXPATHL];
  stl_hlrec_t *hltab;
  stl_hlrec_t **hltab_ptr = highlights ? &hltab : NULL;

  // Temporarily reset 'cursorbind' to prevent side effects from moving the cursor away and back.
  int p_crb_save = ewp->w_p_crb;
  ewp->w_p_crb = false;

  int width = build_stl_str_hl(ewp,
                               (char_u *)buf,
                               sizeof(buf),
                               (char_u *)str.data,
                               false,
                               fillchar,
                               maxwidth,
                               hltab_ptr,
                               NULL);

  PUT(result, "width", INTEGER_OBJ(width));

  // Restore original value of 'cursorbind'
  ewp->w_p_crb = p_crb_save;

  if (highlights) {
    Array hl_values = ARRAY_DICT_INIT;
    const char *grpname;
    char user_group[6];

    // If first character doesn't have a defined highlight,
    // add the default highlight at the beginning of the highlight list
    if (hltab->start == NULL || ((char *)hltab->start - buf) != 0) {
      Dictionary hl_info = ARRAY_DICT_INIT;
      grpname = get_default_stl_hl(wp);

      PUT(hl_info, "start", INTEGER_OBJ(0));
      PUT(hl_info, "group", CSTR_TO_OBJ(grpname));

      ADD(hl_values, DICTIONARY_OBJ(hl_info));
    }

    for (stl_hlrec_t *sp = hltab; sp->start != NULL; sp++) {
      Dictionary hl_info = ARRAY_DICT_INIT;

      PUT(hl_info, "start", INTEGER_OBJ((char *)sp->start - buf));

      if (sp->userhl == 0) {
        grpname = get_default_stl_hl(wp);
      } else if (sp->userhl < 0) {
        grpname = (char *)syn_id2name(-sp->userhl);
      } else {
        snprintf(user_group, sizeof(user_group), "User%d", sp->userhl);
        grpname = user_group;
      }

      PUT(hl_info, "group", CSTR_TO_OBJ(grpname));

      ADD(hl_values, DICTIONARY_OBJ(hl_info));
    }

    PUT(result, "highlights", ARRAY_OBJ(hl_values));
  }

  PUT(result, "str", CSTR_TO_OBJ((char *)buf));

  return result;
}

/// Create a new user command |user-commands|
///
/// {name} is the name of the new command. The name must begin with an uppercase letter.
///
/// {command} is the replacement text or Lua function to execute.
///
/// Example:
/// <pre>
///    :call nvim_add_user_command('SayHello', 'echo "Hello world!"', {})
///    :SayHello
///    Hello world!
/// </pre>
///
/// @param  name    Name of the new user command. Must begin with an uppercase letter.
/// @param  command Replacement command to execute when this user command is executed. When called
///                 from Lua, the command can also be a Lua function. The function is called with a
///                 single table argument that contains the following keys:
///                 - args: (string) The args passed to the command, if any |<args>|
///                 - bang: (boolean) "true" if the command was executed with a ! modifier |<bang>|
///                 - line1: (number) The starting line of the command range |<line1>|
///                 - line2: (number) The final line of the command range |<line2>|
///                 - range: (number) The number of items in the command range: 0, 1, or 2 |<range>|
///                 - count: (number) Any count supplied |<count>|
///                 - reg: (string) The optional register, if specified |<reg>|
///                 - mods: (string) Command modifiers, if any |<mods>|
/// @param  opts    Optional command attributes. See |command-attributes| for more details. To use
///                 boolean attributes (such as |:command-bang| or |:command-bar|) set the value to
///                 "true". In addition to the string options listed in |:command-complete|, the
///                 "complete" key also accepts a Lua function which works like the "customlist"
///                 completion mode |:command-completion-customlist|. Additional parameters:
///                 - desc: (string) Used for listing the command when a Lua function is used for
///                                  {command}.
///                 - force: (boolean, default true) Override any previous definition.
/// @param[out] err Error details, if any.
void nvim_add_user_command(String name, Object command, Dict(user_command) *opts, Error *err)
  FUNC_API_SINCE(9)
{
  add_user_command(name, command, opts, 0, err);
}

/// Delete a user-defined command.
///
/// @param  name    Name of the command to delete.
/// @param[out] err Error details, if any.
void nvim_del_user_command(String name, Error *err)
  FUNC_API_SINCE(9)
{
  nvim_buf_del_user_command(-1, name, err);
}
