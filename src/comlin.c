// Copyright 2023 David Robillard <d@drobilla.net>
// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

/* Makes a number of assumptions that happen to be true on virtually any
 * remotely modern POSIX system.
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * TODO:
 * - Add Win32 support.
 */

#include "comlin/comlin.h"

#ifdef _WIN32
#    include <io.h>
#    include <windows.h>
#else // POSIX
#    include <sys/ioctl.h>
#    include <sys/types.h>
#    include <termios.h>
#    include <unistd.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef O_CLOEXEC
#    define O_CLOEXEC 0
#endif

// The two characters that begin a VT-100 escape sequence: `ESC [`
#define VTESC "\x1B["

#ifdef _WIN32
typedef int ssize_t; // read() and write() return value

typedef struct {
    DWORD in_mode;  ///< Console input mode
    DWORD out_mode; ///< Console output mode
} ComlinTerminalState;

#else // POSIX
typedef struct termios ComlinTerminalState;
#endif

// A resizable buffer that contains a string
typedef struct {
    char* data;    ///< Pointer to string buffer
    size_t length; ///< Length of string (not greater than size)
    size_t size;   ///< Size of data
} StringBuf;

struct ComlinStateImpl {
    // Completion
    ComlinCompletionCallback* completion_callback; ///< Get completions

    // Terminal session state
    int ifd;       ///< Terminal stdin file descriptor
    int ofd;       ///< Terminal stdout file descriptor
    size_t cols;   ///< Number of columns in terminal
    bool maskmode; ///< Show asterisks instead of input (for passwords)
    bool rawmode;  ///< Terminal is currently in raw mode
    bool mlmode;   ///< Multi-line mode (default is single line)
    bool dumb;     ///< True if terminal is unsupported (no features)

    // History
    size_t history_max_len; ///< Maximum number of history entries to keep
    size_t history_len;     ///< Number of history entries
    char** history;         ///< History entries

    // Terminal state
    ComlinTerminalState cooked; ///< Terminal settings before raw mode

    // Line editing state
    StringBuf buf;         ///< Editing line buffer
    char const* prompt;    ///< Prompt to display
    size_t plen;           ///< Prompt length
    size_t pos;            ///< Current cursor position
    size_t history_index;  ///< The history index we're currently editing
    bool in_completion;    ///< Currently doing a completion
    size_t completion_idx; ///< Index of next completion to propose

    // Multi-line refresh state
    size_t oldpos;  ///< Previous refresh cursor position
    size_t oldrows; ///< Rows used by last refreshed line (multi-line)
};

static char const* const unsupported_term[] = {"dumb", "cons25", "emacs", NULL};

static void
buf_append(StringBuf* buf, char const* s, size_t len);

static ComlinStatus
refresh_line_with_completion(ComlinState* ls,
                             ComlinCompletions const* lc,
                             unsigned flags);

static ComlinStatus
refresh_line_with_flags(ComlinState* l, unsigned flags);

typedef enum {
    CTRL_C = 3,   // ^C (ETX)
    CTRL_D = 4,   // ^D (EOT)
    TAB = 9,      // ^I (HT) - Tab
    LFEED = 10,   // ^J (LF) - Usually "Enter" or "Return"
    CRETURN = 13, // ^M (CR) - Carriage Return
    ESC = 27,     // ^[ (ESC)
    DEL = 127     // ^? (DEL) - Usually "Backspace"
} ControlCharacter;

typedef unsigned ComlinRefreshFlags;

static ComlinRefreshFlags const REFRESH_CLEAN = 1U << 0U;
static ComlinRefreshFlags const REFRESH_WRITE = 1U << 1U;
static ComlinRefreshFlags const REFRESH_ALL = 3U;

static ComlinStatus
comlin_edit_refresh(ComlinState* l);

/* Terminal Communication */

// Return true if a `real` TERM value matches an `ideal` one
static bool
term_matches(char const* const ideal, char const* const real)
{
    // Intentionally loose: match anything with the ideal prefix
    for (unsigned i = 0U; ideal[i]; ++i) {
        char const r = real[i];
        char const l = (char)((r >= 'A' && r <= 'Z') ? (r + ('a' - 'A')) : r);
        if (l != ideal[i]) {
            return false;
        }
    }

    return true;
}

// Return true if the terminal is known to not support basic escape seequences
static bool
is_unsupported_term(char const* const term)
{
    if (term) {
        for (unsigned i = 0U; unsupported_term[i]; ++i) {
            if (term_matches(unsupported_term[i], term)) {
                return true;
            }
        }
    }

    return false;
}

#ifdef _WIN32
static HANDLE
console_handle(int const fd)
{
    return _isatty(fd) ? (HANDLE)_get_osfhandle(fd) : (HANDLE)0;
}
#endif

static ComlinStatus
read_char(int const fd, char* const buf)
{
    ssize_t const r = read(fd, buf, 1);

    return r < 0 ? COMLIN_BAD_READ : r == 0 ? COMLIN_END : COMLIN_SUCCESS;
}

static ComlinStatus
write_string(int const fd, char const* const buf, size_t const count)
{
    size_t offset = 0U;
    while (offset < count) {
        ssize_t const r = write(fd, buf + offset, count - offset);
        if (r < 0) {
            return COMLIN_BAD_WRITE;
        }

        offset += (size_t)r;
    }

    return COMLIN_SUCCESS;
}

// Set terminal to raw input mode and preserve the original settings
static ComlinStatus
enable_raw_mode(ComlinState* const state)
{
#ifdef _WIN32
    _setmode(state->ifd, _O_BINARY);
    _setmode(state->ofd, _O_BINARY);

    HANDLE const ih = console_handle(state->ifd);
    if (ih && (!GetConsoleMode(ih, &state->cooked.in_mode) ||
               !SetConsoleMode(
                 ih, state->cooked.in_mode | ENABLE_VIRTUAL_TERMINAL_INPUT))) {
        return COMLIN_BAD_TERMINAL;
    }

    HANDLE const oh = console_handle(state->ofd);
    if (oh && (!GetConsoleMode(oh, &state->cooked.out_mode) ||
               !SetConsoleMode(oh,
                               state->cooked.out_mode |
                                 ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                 DISABLE_NEWLINE_AUTO_RETURN))) {
        return COMLIN_BAD_TERMINAL;
    }

    state->rawmode = true;
    return COMLIN_SUCCESS;

#else // POSIX
    if (!isatty(state->ifd)) {
        return COMLIN_SUCCESS;
    }

    // Save original terminal attributes
    if (tcgetattr(state->ifd, &state->cooked) == -1) {
        return COMLIN_BAD_TERMINAL;
    }

    struct termios raw = state->cooked;
    raw.c_iflag &= ~(tcflag_t)BRKINT; // No break
    raw.c_iflag &= ~(tcflag_t)ICRNL;  // No CR to NL
    raw.c_iflag &= ~(tcflag_t)INPCK;  // No parity check
    raw.c_iflag &= ~(tcflag_t)ISTRIP; // No strip char
    raw.c_iflag &= ~(tcflag_t)IXON;   // No flow control
    raw.c_oflag &= ~(tcflag_t)OPOST;  // No post processing
    raw.c_cflag |= (tcflag_t)CS8;     // 8 bit characters
    raw.c_lflag &= ~(tcflag_t)ECHO;   // No echo
    raw.c_lflag &= ~(tcflag_t)ICANON; // No canonical mode
    raw.c_lflag &= ~(tcflag_t)IEXTEN; // No extended functions
    raw.c_lflag &= ~(tcflag_t)ISIG;   // No signal chars (^Z, ^C)
    raw.c_cc[VMIN] = 1;               // Minimum 1 byte read
    raw.c_cc[VTIME] = 0;              // No read timeout

    // Set raw terminal mode after flushing
    int const rc = tcsetattr(state->ifd, TCSAFLUSH, &raw);
    state->rawmode = !rc;
    return rc ? COMLIN_BAD_TERMINAL : COMLIN_SUCCESS;
#endif
}

static ComlinStatus
disable_raw_mode(ComlinState* const state)
{
    if (state->rawmode) {
#ifdef _WIN32
        HANDLE const ih = console_handle(state->ifd);
        HANDLE const oh = console_handle(state->ofd);
        if ((ih && !SetConsoleMode(ih, state->cooked.in_mode)) ||
            (oh && !SetConsoleMode(oh, state->cooked.out_mode))) {
            return COMLIN_BAD_TERMINAL;
        }
#else // POSIX
        if (tcsetattr(state->ifd, TCSAFLUSH, &state->cooked) == -1) {
            return COMLIN_BAD_TERMINAL;
        }
#endif

        state->rawmode = false;
    }

    return COMLIN_SUCCESS;
}

// Get the cursor position by communicating with the terminal
static int
get_cursor_position(int const ifd, int const ofd)
{
    // Send request for cursor location
    if (write_string(ofd, VTESC "6n", 5)) {
        return -1;
    }

    // Read start of response: ESC [
    char buf[32] = {0};
    if (read_char(ifd, &buf[0]) || buf[0] != ESC || //
        read_char(ifd, &buf[1]) || buf[1] != '[') {
        return -1;
    }

    // Read response body: rows ; cols R
    unsigned int i = 2;
    while (i < sizeof(buf) && !read_char(ifd, buf + i) && buf[i] != 'R') {
        ++i;
    }

    buf[i] = '\0';
    errno = 0;

    // Skip the number of rows
    i = 2;
    while (buf[i] >= '0' && buf[i] <= '9') {
        ++i;
    }
    if (buf[i] != ';') {
        return -1;
    }

    // Parse the number of columns
    char* end = NULL;
    long const cols = strtol(buf + i + 1, &end, 10);
    if (errno || cols < 1 || *end != '\0') {
        return -1;
    }

    return (int)cols;
}

// Get the number of columns in the terminal, or fall back to 80
static int
get_columns(ComlinState* const state)
{
#ifdef _WIN32
    intptr_t const oh = _get_osfhandle(state->ofd);
    CONSOLE_SCREEN_BUFFER_INFO b;
    return GetConsoleScreenBufferInfo((HANDLE)oh, &b)
             ? (b.srWindow.Right - b.srWindow.Left)
             : 80;
#else
    int const ifd = state->ifd;
    int const ofd = state->ofd;
    struct winsize ws = {24U, 80U, 640U, 480U};

    if (isatty(ofd) && (ioctl(ofd, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)) {
        // ioctl() failed. Try to query the terminal itself
        ws.ws_col = 80U;
        enable_raw_mode(state);
        if (!write_string(ofd, VTESC "999C", 6)) { // Go to the right margin
            int const cols = get_cursor_position(ifd, ofd); // Get the column
            write_string(ofd, "\r", 1); // Return to the left margin
            ws.ws_col = cols > 0 ? (unsigned short)cols : 80U;
        }
        disable_raw_mode(state);
    }

    return ws.ws_col;
#endif
}

ComlinStatus
comlin_clear_screen(ComlinState* const state)
{
    return write_string(state->ofd, VTESC "H" VTESC "2J", 7);
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void
comlin_beep(ComlinState const* const state)
{
    if (write(state->ofd, "\x07", 1) != 1) {
        // Failed to write ASCII BEL, oh well
    }
}

/* Completion */

// Free a list of completion option populated by comlin_add_completion()
static void
free_completions(ComlinCompletions* const lc)
{
    for (size_t i = 0U; i < lc->len; ++i) {
        free(lc->cvec[i]);
    }
    if (lc->cvec != NULL) {
        free(lc->cvec);
    }
}

// Show the current line with the proposed completion
static ComlinStatus
refresh_line_with_completion(ComlinState* const ls,
                             ComlinCompletions const* const lc,
                             unsigned const flags)
{
    // Show the edited line with completion if possible, or just refresh
    if (ls->completion_idx < lc->len) {
        size_t const saved_pos = ls->pos;
        StringBuf const saved_buf = ls->buf;
        ls->buf.data = lc->cvec[ls->completion_idx];
        ls->pos = ls->buf.length = strlen(ls->buf.data);
        refresh_line_with_flags(ls, flags);
        ls->buf = saved_buf;
        ls->pos = saved_pos;
        return COMLIN_SUCCESS;
    }
    return refresh_line_with_flags(ls, flags);
}

/* Helper for when the user presses Tab, or another key during completion.
 *
 * If the return is non-zero, it should be handled as a byte read from the
 * input, and process it as usual.  Otherwise, the character was consumed by
 * this function.
 */
static char
complete_line(ComlinState* const ls, char const keypressed)
{
    ComlinCompletions lc = {0, NULL};
    char c = keypressed;

    if (ls->buf.length) {
        ls->completion_callback(ls->buf.data, &lc);
    }

    if (lc.len == 0) {
        comlin_beep(ls);
        ls->in_completion = false;
    } else {
        switch (c) {
        case TAB:
            if (!ls->in_completion) {
                ls->in_completion = true;
                ls->completion_idx = 0;
            } else {
                ls->completion_idx = (ls->completion_idx + 1) % (lc.len + 1);
                if (ls->completion_idx == lc.len) {
                    comlin_beep(ls);
                }
            }
            c = 0;
            break;
        case ESC:
            // Re-show original buffer
            if (ls->completion_idx < lc.len) {
                comlin_edit_refresh(ls);
            }
            ls->in_completion = false;
            c = 0;
            break;
        default:
            // Update buffer and return
            if (ls->completion_idx < lc.len) {
                ls->pos = strlen(lc.cvec[ls->completion_idx]);
                ls->buf.length = 0U;
                buf_append(&ls->buf, lc.cvec[ls->completion_idx], ls->pos);
            }
            ls->in_completion = false;
            break;
        }

        // Show completion or original buffer
        if (ls->in_completion) {
            refresh_line_with_completion(ls, &lc, REFRESH_ALL);
        } else {
            comlin_edit_refresh(ls);
        }
    }

    free_completions(&lc);
    return c; // Return last read character
}

void
comlin_set_completion_callback(ComlinState* const state,
                               ComlinCompletionCallback* const fn)
{
    state->completion_callback = fn;
}

static char*
comlin_copy_string(char const* const str)
{
    size_t const len = strlen(str);
    size_t const total_size = len + 1U;
    char* const copy = (char*)malloc(total_size);

    if (copy) {
        memcpy(copy, str, total_size);
    }

    return copy;
}

ComlinStatus
comlin_add_completion(ComlinCompletions* const lc, char const* const str)
{
    size_t const cvec_size = sizeof(char*) * (lc->len + 1U);
    char** const cvec = (char**)realloc(lc->cvec, cvec_size);
    if (!cvec) {
        return COMLIN_NO_MEMORY;
    }

    lc->cvec = cvec;

    char* const copy = comlin_copy_string(str);
    if (!copy) {
        return COMLIN_NO_MEMORY;
    }

    lc->cvec[lc->len++] = copy;
    return COMLIN_SUCCESS;
}

/* String Buffer */

static void
buf_append(StringBuf* const buf, char const* const s, size_t const len)
{
    assert(s);

    size_t const new_length = buf->length + len;
    size_t const needed_size = new_length + 1U;
    if (needed_size > buf->size) {
        char* const new_data = (char*)realloc(buf->data, needed_size);
        if (!new_data) {
            return;
        }

        buf->data = new_data;
        buf->size = needed_size;
    }

    assert(buf->data);
    memcpy(buf->data + buf->length, s, len);
    buf->data[new_length] = '\0';
    buf->length = new_length;
}

static size_t
format_size(char* const buf, size_t x)
{
    size_t i = 0U;

    // Write digits backwards
    do {
        buf[i++] = (char)('0' + (x % 10U));
        x /= 10U;
    } while (x);

    // Terminate and reverse string
    buf[i] = '\0';
    if (i) {
        for (size_t l = 0U, r = i - 1U; l < r; ++l, --r) {
            char const t = buf[l];
            buf[l] = buf[r];
            buf[r] = t;
        }
    }

    return i;
}

// Append an escape like `ESC [ n s` with a number and a suffix letter
static void
buf_append_vtesc(StringBuf* const buf, size_t const num, char const suffix)
{
    size_t end = 2U;
    char seq[64] = {'\x1B', '[', 0};

    end += format_size(seq + 2U, num);
    seq[end++] = suffix;

    buf_append(buf, seq, end);
}

static void
buf_free(StringBuf* const buf)
{
    free(buf->data);
}

static void
append_line_text(StringBuf* const buf,
                 char const* const text,
                 size_t const length,
                 bool const masked)
{
    if (masked) {
        for (size_t i = 0U; i < length; ++i) {
            buf_append(buf, "*", 1);
        }
    } else {
        buf_append(buf, text, length);
    }
}

/* Refresh */

// Clear and refresh the current line in single-line mode
static ComlinStatus
refresh_single_line(ComlinState const* const l, ComlinRefreshFlags const flags)
{
    // Chop the start if necessary so the cursor is on screen
    char* buf = l->buf.data;
    size_t len = l->buf.length;
    size_t pos = l->pos;
    if (l->plen + l->pos >= l->cols) {
        size_t const offset = l->plen + l->pos + 1U - l->cols;
        buf += offset;
        len -= offset;
        pos -= offset;
    }

    // Truncate display length to fit on the row
    if (l->plen + len > l->cols) {
        len = l->cols - l->plen;
    }

    // Start building an update for the whole row (to be sent in one write)
    StringBuf update = {NULL, 0U, 0U};
    buf_append(&update, "\r", 1);

    if (flags & REFRESH_WRITE) {
        // Write the prompt and the current buffer content
        buf_append(&update, l->prompt, l->plen);
        append_line_text(&update, buf, len, l->maskmode);
    }

    // Erase to right
    buf_append(&update, VTESC "0K", 4);

    if (flags & REFRESH_WRITE) {
        // Move cursor to original position
        buf_append(&update, "\r", 1);
        buf_append_vtesc(&update, pos + l->plen, 'C');
    }

    ComlinStatus const st = write_string(l->ofd, update.data, update.length);
    buf_free(&update);
    return st;
}

// Refresh the current line in multi-line mode
static ComlinStatus
refresh_multi_line(ComlinState* const l, ComlinRefreshFlags const flags)
{
    size_t const rpos = (l->plen + l->oldpos + l->cols) / l->cols;
    size_t const old_rows = l->oldrows;
    int const fd = l->ofd;

    // Calculate the total number of rows in the line
    size_t rows = (l->plen + l->buf.length + l->cols - 1U) / l->cols;
    l->oldrows = rows;

    // We'll build the update here, then send it all in a single write
    StringBuf update = {NULL, 0U, 0U};

    // First clear all the old used rows
    if (flags & REFRESH_CLEAN) {
        // Go to the last row
        if (old_rows > rpos) {
            buf_append_vtesc(&update, old_rows - rpos, 'B');
        }

        // For each row, clear it, then move up
        for (size_t j = 1U; j < old_rows; ++j) {
            buf_append(&update, "\r" VTESC "0K" VTESC "1A", 9U);
        }
    }

    if (flags & REFRESH_WRITE) {
        // Write the current prompt and line over the current row
        buf_append(&update, "\r", 1);
        buf_append(&update, l->prompt, l->plen);
        append_line_text(&update, l->buf.data, l->buf.length, l->maskmode);
        buf_append(&update, VTESC "0K", 4U);

        // If we're at the end of the row, move to the start of the next
        if (l->pos && l->pos == l->buf.length &&
            (l->pos + l->plen) % l->cols == 0) {
            buf_append(&update, "\n\r", 2);
            if (++rows > l->oldrows) {
                l->oldrows = rows;
            }
        }

        // Move the cursor up to the correct row if necessary
        size_t const rpos2 = (l->plen + l->pos + l->cols) / l->cols;
        if (rows > rpos2) {
            buf_append_vtesc(&update, rows - rpos2, 'A');
        }

        // Move the cursor to the correct column
        buf_append(&update, "\r", 1);
        size_t const col = (l->plen + l->pos) % l->cols;
        if (col) {
            buf_append_vtesc(&update, col, 'C');
        }
    }

    l->oldpos = l->pos;

    ComlinStatus const st = write_string(fd, update.data, update.length);
    buf_free(&update);
    return st;
}

// Optionally clear and/or refresh the current line
static ComlinStatus
refresh_line_with_flags(ComlinState* const l, ComlinRefreshFlags const flags)
{
    return l->mlmode ? refresh_multi_line(l, flags)
                     : refresh_single_line(l, flags);
}

ComlinStatus
comlin_hide(ComlinState* const l)
{
    return refresh_line_with_flags(l, REFRESH_CLEAN);
}

ComlinStatus
comlin_show(ComlinState* const l)
{
    if (l->in_completion && l->buf.length) {
        ComlinCompletions completions = {0U, NULL};
        l->completion_callback(l->buf.data, &completions);
        return refresh_line_with_completion(l, &completions, REFRESH_WRITE);
    }

    return refresh_line_with_flags(l, REFRESH_WRITE);
}

/* Editing Operations */

static inline ComlinStatus
edit_status(ComlinStatus const st)
{
    return st ? st : COMLIN_EDITING;
}

static ComlinStatus
comlin_edit_refresh(ComlinState* const l)
{
    return edit_status(refresh_line_with_flags(l, REFRESH_ALL));
}

// Insert a character at the current cursor position
static ComlinStatus
comlin_edit_insert(ComlinState* const l, char const c)
{
    if (l->buf.length == l->pos) {
        // Insert at end of line
        buf_append(&l->buf, &c, 1U);
        ++l->pos;
        if ((!l->mlmode || l->oldrows <= 1U) &&
            l->plen + l->buf.length < l->cols) {
            // Avoid a full update of the line in the trivial case
            char const d = (char)(l->maskmode ? '*' : c);
            return write(l->ofd, &d, 1) == 1 ? COMLIN_EDITING
                                             : COMLIN_BAD_WRITE;
        }
    } else {
        // Insert in middle of line
        buf_append(&l->buf, " ", 1U); // Allocate space for one extra character
        memmove(l->buf.data + l->pos + 1,
                l->buf.data + l->pos,
                l->buf.length - l->pos);
        l->buf.data[l->pos] = c;
        ++l->pos;
    }

    return comlin_edit_refresh(l);
}

// Move cursor one column to the left if possible
static ComlinStatus
comlin_edit_move_left(ComlinState* const l)
{
    if (l->pos > 0) {
        --l->pos;
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

// Move cursor one column to the right if possible
static ComlinStatus
comlin_edit_move_right(ComlinState* const l)
{
    if (l->pos != l->buf.length) {
        ++l->pos;
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

// Move cursor to the start of the line
static ComlinStatus
comlin_edit_move_home(ComlinState* const l)
{
    if (l->pos != 0) {
        l->pos = 0;
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

// Move cursor to the end of the line
static ComlinStatus
comlin_edit_move_end(ComlinState* const l)
{
    if (l->pos != l->buf.length) {
        l->pos = l->buf.length;
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

// Transpose the character under the cursor with the previous character
static ComlinStatus
comlin_edit_transpose(ComlinState* const state)
{
    if (state->pos > 0U && state->pos < state->buf.length) {
        char const aux = state->buf.data[state->pos - 1];
        state->buf.data[state->pos - 1] = state->buf.data[state->pos];
        state->buf.data[state->pos] = aux;
        if (state->pos != state->buf.length - 1U) {
            ++state->pos;
        }
        return edit_status(comlin_edit_refresh(state));
    }
    return COMLIN_EDITING;
}

typedef enum {
    COMLIN_HISTORY_NEXT,
    COMLIN_HISTORY_PREV,
} ComlinHistoryDirection;

// Substitute the currently edited line with the next or previous history entry
static ComlinStatus
comlin_edit_history_step(ComlinState* const l, ComlinHistoryDirection const dir)
{
    if (l->history_len > 1U) {
        // Update the current history entry before overwriting it with the next
        size_t const current_index = l->history_len - 1U - l->history_index;
        free(l->history[current_index]);
        l->history[current_index] = comlin_copy_string(l->buf.data);
        if (!l->history[current_index]) {
            return COMLIN_NO_MEMORY;
        }

        // Update the history index
        if (dir == COMLIN_HISTORY_NEXT) {
            if (l->history_index == 0) {
                return COMLIN_EDITING;
            }
            --l->history_index;
        } else {
            if (l->history_index == l->history_len - 1U) {
                return COMLIN_EDITING;
            }
            ++l->history_index;
        }

        // Show the new entry
        size_t const new_index = l->history_len - 1U - l->history_index;
        l->pos = strlen(l->history[new_index]);
        l->buf.length = 0U;
        buf_append(&l->buf, l->history[new_index], l->pos);
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

static ComlinStatus
comlin_edit_history_prev(ComlinState* const l)
{
    return comlin_edit_history_step(l, COMLIN_HISTORY_PREV);
}

static ComlinStatus
comlin_edit_history_next(ComlinState* const l)
{
    return comlin_edit_history_step(l, COMLIN_HISTORY_NEXT);
}

static void
comlin_edit_history_pop(ComlinState* const state)
{
    --state->history_len;
    free(state->history[state->history_len]);
    state->history_index = 0U;
}

// Delete the character to the right of the cursor
static ComlinStatus
comlin_edit_delete(ComlinState* const l)
{
    if (l->pos < l->buf.length) {
        memmove(l->buf.data + l->pos,
                l->buf.data + l->pos + 1U,
                l->buf.length - l->pos - 1U);
        --l->buf.length;
        l->buf.data[l->buf.length] = '\0';
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

static ComlinStatus
comlin_edit_interrupt(ComlinState* const l)
{
    (void)l;
    return COMLIN_INTERRUPTED;
}

static ComlinStatus
comlin_edit_eof(ComlinState* const l)
{
    if (!l->buf.length) {
        comlin_edit_history_pop(l);
        return COMLIN_END;
    }

    return comlin_edit_delete(l);
}

// Backspace implementation
static ComlinStatus
comlin_edit_backspace(ComlinState* const l)
{
    if (l->pos) {
        memmove(l->buf.data + l->pos - 1U,
                l->buf.data + l->pos,
                l->buf.length - l->pos);
        --l->pos;
        --l->buf.length;
        l->buf.data[l->buf.length] = '\0';
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

// Delete the word before the cursor
static ComlinStatus
comlin_edit_delete_prev_word(ComlinState* const l)
{
    size_t const old_pos = l->pos;

    while (l->pos > 0 && l->buf.data[l->pos - 1U] == ' ') {
        --l->pos;
    }
    while (l->pos > 0 && l->buf.data[l->pos - 1U] != ' ') {
        --l->pos;
    }
    size_t const diff = old_pos - l->pos;
    memmove(l->buf.data + l->pos,
            l->buf.data + old_pos,
            l->buf.length + 1U - old_pos);
    l->buf.length -= diff;
    return comlin_edit_refresh(l);
}

static ComlinStatus
comlin_edit_clear_screen(ComlinState* const state)
{
    ComlinStatus const st = comlin_clear_screen(state);

    return st ? st : edit_status(comlin_edit_refresh(state));
}

static ComlinStatus
comlin_edit_clear_line_backwards(ComlinState* const l)
{
    if (l->pos > 0) {
        size_t const new_length = l->buf.length - l->pos;
        memmove(l->buf.data, l->buf.data + l->pos, new_length + 1);
        l->buf.length = new_length;
        l->pos = 0;
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

static ComlinStatus
comlin_edit_clear_line_forwards(ComlinState* const l)
{
    if (l->pos < l->buf.length) {
        l->buf.data[l->pos] = '\0';
        l->buf.length = l->pos;
        return comlin_edit_refresh(l);
    }
    return COMLIN_EDITING;
}

static ComlinStatus
comlin_edit_submit(ComlinState* const l)
{
    comlin_edit_history_pop(l);
    if (l->mlmode) {
        comlin_edit_move_end(l);
    }
    l->buf.data[l->buf.length] = '\0';
    return COMLIN_SUCCESS;
}

/* State */

ComlinState*
comlin_new_state(int const in_fd,
                 int const out_fd,
                 char const* const term,
                 size_t const max_history_len)
{
    ComlinState* const l = (ComlinState*)calloc(1, sizeof(ComlinState));
    if (l) {
        l->ifd = in_fd;
        l->ofd = out_fd;
        l->dumb = is_unsupported_term(term);
        l->history_max_len = max_history_len;
    }
    return l;
}

void
comlin_free_state(ComlinState* const state)
{
    // Free history
    for (size_t j = 0U; j < state->history_len; ++j) {
        free(state->history[j]);
    }
    free(state->history);

    // Disable raw mode if it was enabled by comlin_new_state
    disable_raw_mode(state);

    free(state->buf.data);
    free(state);
}

ComlinStatus
comlin_set_mode(ComlinState* const state, ComlinModeFlags const flags)
{
    state->mlmode = flags & (ComlinModeFlags)COMLIN_MODE_MULTI_LINE;
    state->maskmode = flags & (ComlinModeFlags)COMLIN_MODE_MASKED;
    return COMLIN_SUCCESS;
}

ComlinStatus
comlin_edit_start(ComlinState* const l, char const* const prompt)
{
    // Enter raw mode
    ComlinStatus const st = enable_raw_mode(l);
    if (st) {
        return st;
    }

    // Reset line state
    l->pos = 0U;
    l->oldpos = 0U;
    l->buf.length = 0U;
    l->oldrows = 0U;
    if (!l->cols) {
        l->cols = (size_t)get_columns(l);
        if (l->buf.size < l->cols) {
            free(l->buf.data);
            l->buf.data = (char*)calloc(1, l->cols);
            l->buf.size = l->cols;
        }
    }

    // Set edit state
    l->prompt = prompt;
    l->plen = strlen(prompt);
    l->buf.data[0] = '\0';
    comlin_history_add(l, ""); // Latest history entry is the current line

    // Write prompt
    return write_string(l->ofd, l->prompt, l->plen);
}

static ComlinStatus
comlin_edit_read_dumb(ComlinState* const l, char const c)
{
    switch (c) {
    case CTRL_C:
        return COMLIN_INTERRUPTED;
    case CTRL_D:
        return COMLIN_END;
    case LFEED:
    case CRETURN:
        return COMLIN_SUCCESS;
    default:
        break;
    }

    write(l->ofd, &c, 1U);
    buf_append(&l->buf, &c, 1U);
    return COMLIN_EDITING;
}

static ComlinStatus
comlin_edit_read_escape(ComlinState* l);

static ComlinStatus
comlin_edit_control(ComlinState* const state, char const c)
{
    typedef ComlinStatus (*ControlHandler)(ComlinState*);

    static ControlHandler const control_handlers[0x20U] = {
      NULL,                             // ^@
      comlin_edit_move_home,            // ^A
      comlin_edit_move_left,            // ^B
      comlin_edit_interrupt,            // ^C
      comlin_edit_eof,                  // ^D
      comlin_edit_move_end,             // ^E
      comlin_edit_move_right,           // ^F
      NULL,                             // ^G
      comlin_edit_backspace,            // ^H
      NULL,                             // ^I
      comlin_edit_submit,               // ^J
      comlin_edit_clear_line_forwards,  // ^K
      comlin_edit_clear_screen,         // ^L
      comlin_edit_submit,               // ^M
      comlin_edit_history_next,         // ^N
      NULL,                             // ^O
      comlin_edit_history_prev,         // ^P
      NULL,                             // ^Q
      NULL,                             // ^R
      NULL,                             // ^S
      comlin_edit_transpose,            // ^T
      comlin_edit_clear_line_backwards, // ^U
      NULL,                             // ^V
      comlin_edit_delete_prev_word,     // ^W
      NULL,                             // ^X
      NULL,                             // ^Y
      NULL,                             // ^Z
      comlin_edit_read_escape,          // ^[
      NULL,                             // ^Backslash
      NULL,                             // ^]
      NULL,                             // ^^
      NULL,                             // ^_
    };

    ControlHandler const handler = control_handlers[(unsigned)c];
    return handler ? handler(state) : COMLIN_EDITING;
}

ComlinStatus
comlin_edit_feed(ComlinState* const l)
{
    // Read the next character
    char c = '\0';
    ComlinStatus const st = read_char(l->ifd, &c);
    if (st) {
        return st;
    }

    if (l->dumb) {
        return comlin_edit_read_dumb(l, c); // Fallback for dumb terminals
    }

    if ((l->in_completion || c == TAB) && l->completion_callback) {
        // Try to autocomplete
        c = complete_line(l, c);
        if (c < 0) {
            return COMLIN_BAD_READ;
        }
        if (c == 0) {
            return COMLIN_EDITING;
        }
    }

    return (c < 0x20)   ? comlin_edit_control(l, c)
           : (c == DEL) ? comlin_edit_backspace(l)
                        : comlin_edit_insert(l, c);
}

static ComlinStatus
comlin_edit_read_escape(ComlinState* const l)
{
    // Read the next two bytes representing the escape sequence
    char seq[4] = {'\0', '\0', '\0', '\0'};
    if (read_char(l->ifd, &seq[0]) || read_char(l->ifd, &seq[1])) {
        return COMLIN_BAD_READ;
    }

    if (seq[0] == '[') { // ESC [ sequences
        if (seq[1] >= '0' && seq[1] <= '9') {
            // Extended escape, read additional byte
            return read_char(l->ifd, &seq[2])         ? COMLIN_BAD_READ
                   : (seq[1] == '3' && seq[2] == '~') ? comlin_edit_delete(l)
                                                      : COMLIN_SUCCESS;
        }

        switch (seq[1]) {
        case 'A': // Up
            return comlin_edit_history_prev(l);
        case 'B': // Down
            return comlin_edit_history_next(l);
        case 'C': // Right
            return comlin_edit_move_right(l);
        case 'D': // Left
            return comlin_edit_move_left(l);
        case 'H': // Home
            return comlin_edit_move_home(l);
        case 'F': // End
            return comlin_edit_move_end(l);
        default:
            break;
        }

    } else if (seq[0] == 'O') { // ESC O sequence
        switch (seq[1]) {
        case 'H': // Home
            return comlin_edit_move_home(l);
        case 'F': // End
            return comlin_edit_move_end(l);
        default:
            break;
        }
    }

    return COMLIN_SUCCESS;
}

ComlinStatus
comlin_edit_stop(ComlinState* const l)
{
    ComlinStatus const st = disable_raw_mode(l);

    return st ? st : write_string(l->ofd, "\n", 1);
}

char const*
comlin_text(ComlinState const* const l)
{
    return l->buf.data;
}

ComlinStatus
comlin_read_line(ComlinState* const state, char const* const prompt)
{
    ComlinStatus st0 = comlin_edit_start(state, prompt);
    ComlinStatus st1 = COMLIN_SUCCESS;
    if (!st0) {
        do {
            st0 = comlin_edit_feed(state);
        } while (st0 == COMLIN_EDITING);

        st1 = comlin_edit_stop(state);
    }
    return st0 ? st0 : st1;
}

/* History */

/* Uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
ComlinStatus
comlin_history_add(ComlinState* const state, char const* const line)
{
    if (state->history_max_len == 0) {
        return COMLIN_SUCCESS;
    }

    // Initialization on first call
    if (!state->history) {
        state->history = (char**)malloc(sizeof(char*) * state->history_max_len);
        if (!state->history) {
            return COMLIN_NO_MEMORY;
        }
        memset(state->history, 0, (sizeof(char*) * state->history_max_len));
    }

    // Don't add duplicated lines
    if (state->history_len &&
        !strcmp(state->history[state->history_len - 1U], line)) {
        return COMLIN_SUCCESS;
    }

    // Add an heap allocated copy of the line in the history
    char* const linecopy = comlin_copy_string(line);
    if (!linecopy) {
        return COMLIN_NO_MEMORY;
    }

    // If we reached the max length, remove the older line
    if (state->history_len == state->history_max_len) {
        free(state->history[0]);
        memmove(state->history,
                state->history + 1,
                sizeof(char*) * (state->history_max_len - 1U));
        --state->history_len;
    }

    state->history[state->history_len] = linecopy;
    ++state->history_len;
    return COMLIN_SUCCESS;
}

ComlinStatus
comlin_history_save(ComlinState const* const state, char const* const filename)
{
#ifdef _WIN32
    static int const mode = S_IREAD | S_IWRITE;
#else
    static mode_t const mode = S_IRUSR | S_IWUSR;
#endif

    ComlinStatus st = COMLIN_SUCCESS;
    int const flags = O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC;
    int const fd = open(filename, flags, mode);
    if (fd < 0) {
        return COMLIN_NO_FILE;
    }

    for (size_t j = 0U; !st && j < state->history_len; ++j) {
        size_t const len = strlen(state->history[j]);
        if (len) {
            st = write_string(fd, state->history[j], len);
            if (!st) {
                st = write_string(fd, "\n", 1U);
            }
        }
    }

    return close(fd) < 0 ? COMLIN_BAD_WRITE : st;
}

ComlinStatus
comlin_history_load(ComlinState* const state, char const* const filename)
{
    ComlinStatus st = COMLIN_SUCCESS;
    StringBuf buf = {NULL, 0U, 0U};
    int const fd = open(filename, O_CLOEXEC | O_RDONLY);
    if (fd < 0) {
        return COMLIN_NO_FILE;
    }

    while (!st) {
        char c = '\0';
        st = read_char(fd, &c);

        if (st == COMLIN_SUCCESS) {
            if (c == '\n' && buf.length) {
                comlin_history_add(state, buf.data);
                buf.length = 0U;
            } else if (c >= 0x20 && c != DEL) {
                buf_append(&buf, &c, 1U);
            }
        } else if (st == COMLIN_END) {
            st = COMLIN_SUCCESS;
            break;
        }
    }

    buf_free(&buf);
    return close(fd) < 0 ? COMLIN_BAD_READ : st;
}
