/*** includes ***/
//using macros!
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE


#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.7"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)
// macro bitwise-ANDs a character with the value 00011111
// Upper 3 bits of the character set to 0 (Ctrl in terminal): strips bits 5 and 6

enum editorKey {
    BACKSPACE = 127, // because it doesnt have human-readable backslasl representation in C
    ARROW_LEFT = 1000, // large for no conflict
    ARROW_RIGHT, // 1001 ...
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorHighlight
{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/*** data ***/

struct editorSyntax
{
    char *filetype; //  name of the filetype that will be displayed to the user in the status bar
    char **filematch; // array of strings, where each string contains a pattern to match a filename against
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;        // bit field that will contain flags for whether to highlight numbers and whether to highlight strings for that filetype
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

struct editorConfig {
    int cx, cy; // cursor column, cursor row
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios; // original terminal
};

struct editorConfig E;

/*** filetypes  ***/

char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL};

struct editorSyntax HLDB[] = {
    {"c",
     C_HL_extensions,
     C_HL_keywords,
     "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0])) // highlight database

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char*, int));

/*** terminal ***/

void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);


    perror(s); // prints descriptive error message, also string before error provided
    exit(1); // failure
}

void disableRawMode(){
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
        die("tcsetattr");
    }

    //TCSAFLUSH discards unread input
}

void enableRawMode() {
    // read attributes:
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1){
        die("tcgetattr");
    }
    atexit(disableRawMode); // disableRawMode to be called when 
    // the program exits

    struct termios raw = E.orig_termios; // copy of og

    //raw.c_lflag &= ~(ECHO);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // turn off Ctrl S and Ctrl q (which stop procesisng data in <->)
    // input flag carriage return new line; Ctrl M Off
    raw.c_oflag &= ~(OPOST); // turn off "\n" -> "\r\n"
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON |IEXTEN | ISIG); 
    // IEXTEN is for Ctrl V; disabled.
    // Ctrl C -> SIGINT -> termination; Ctrl Z -> SIGTSTP -> suspend; off
    // ICON to turn off canonical mode, | or
    // ECHO is a BITFLAG. ~ NOT for switchng and AND (&) for flipping
    // common way to turn to 0
    // ECHO causes each key you type to be printed to the terminal
    // useful in canonical mode but not for our UI. turn it off.
    // i.e typing passwords in terminals (sudo)

    raw.c_cc[VMIN] = 0; // minimum number of inputs needed  before read() returns = 0
    raw.c_cc[VTIME] = 1; // maximum amount of time to wait before read() returns = 1/10 sec.

    // apply attributes
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1){
        die("tcsetattr");
    }
    // TCSAFLUSH specifies when to apply changes (here when having 
    //printed output)
}

int editorReadKey(){
    int nread;
    char c;
    // waits for one keypress, else returns (if not error)
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if (nread == -1 && errno !=  EAGAIN) die("read");
    }

    if (c == '\x1b'){
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        // this is for moving with arrow keys. Bytes in form of Escape and 'A ...
        if (seq[0] == '['){
            // For Page Up and Down
            if (seq[1] >= '0' && seq[1] <= '9'){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~'){
                    switch(seq[1]){
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP; // <esc>[5~
                        case '6': return PAGE_DOWN; // <esc>[6~
                        case '7': return HOME_KEY;
                        case '8': return END_KEY; // depending on terminal, may be different!
                    }
                }
            } else {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        } else if (seq[0] =='O'){ // other possible sequence of of terminal
            switch(seq[1]){
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }   else {
        return c;
    }

    
}

int getCursorPosition(int *rows, int *cols){
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    // we're parsing the response
    while (i < sizeof(buf) - 1){
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1; // respond w escape sequence?
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}


int getWindowSize(int *rows, int *cols){
    struct winsize ws;
    // ioctl gets size of terminal
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        // C for Cursor Forward, and B for Cursor Down, 999 for large values to ensure
        return getCursorPosition(rows, cols);
    } else { // success! we have n of rows and cols in our struct!
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

int is_separator(int c)
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
    // strchr returns a pointer to the matching character in the string
}

void editorUpdateSyntax(erow *row)
{
    row->hl = realloc(row->hl, row->rsize); // get needed memory
    memset(row->hl, HL_NORMAL, row->rsize); // set all characters to NORMAL

    if (E.syntax == NULL)
        return;

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1; // keeps track of whether the previous character was a separator.
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize)
    {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment)
        {
            if (!strncmp(&row->render[i], scs, scs_len))
            {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string)
        {
            if (in_comment)
            {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len))
                {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                else
                {
                    i++;
                    continue;
                }
            }
            else if (!strncmp(&row->render[i], mcs, mcs_len))
            {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS)
        {
            if (in_string)
            {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize)
                {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string)
                    in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            }
            else
            {
                if (c == '"' || c == '\'')
                {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {

            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER))
            {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep)
        {
            int j;
            for (j = 0; keywords[j]; j++)
            {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2)
                    klen--;
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen]))
                {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL)
            {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
        }

        int changed = (row->hl_open_comment != in_comment);
        row->hl_open_comment = in_comment;
        if (changed && row->idx + 1 < E.numrows)
            editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl)
{
    switch (hl)
    {
    case HL_COMMENT:
    case HL_MLCOMMENT:
        return 36; // cyan
    case HL_KEYWORD1:
        return 33; // yellow
    case HL_KEYWORD2:
        return 32; // green
    case HL_STRING:
        return 35; // magenta
    case HL_NUMBER:
        return 31; // ANSi color red
    case HL_MATCH:
        return 34;
    default:
        return 37; // Default ANSi color
    }
}

// tries to match the current filename to one of the filematch fields in the HLDB
void editorSelectSyntaxHighlight()
{
    E.syntax = NULL;
    if (E.filename == NULL)
        return;
    char *ext = strrchr(E.filename, '.'); // strrchr() returns a pointer to the last occurrence of a character in a string
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++)
    {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i])
        {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i])))
            {
                E.syntax = s;

                int filerow;
                for (filerow = 0; filerow < E.numrows; filerow++)
                {
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

// editorRowCxToRx converts a chars index into a render index
int editorRowCxToRx(erow *row, int cx){
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++){
        if (row->chars[j] =='\t') // we figure out how many spaces each tab takes
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row , int rx){
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP); // add tab spaces!
        cur_rx++;
        if (cur_rx > rx)
            return cx;
    }
    return cx;
}


void editorUpdateRow(erow *row){
    int tabs = 0;
    int j = 0;
    for (j = 0; j < row->size; j++){
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1); // max for a tab is 8

    int idx = 0;
    for (j = 0; j < row->size; j++){
        if (row->chars[j] =='\t'){
            row->render[idx++] = ' '; // this code is to add spaces when input is tab.
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}


void editorInsertRow(int at, char *s, size_t len){
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++)
        E.row[j].idx++;

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++)
        E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

// editorRowInsertChar() inserts a single character into an erow at a given position
void editorRowInsertChar(erow *row, int at, int c){
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2); // 1 more byte for erow and \0
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at){
    if (at < 0 || at >= row->size) return; // we move to overwrite deleted character
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

// editorInsertChar() tales a char and will insert it into the position of the cursor
void editorInsertChar(int c){
    if (E.cy == E.numrows){
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline(){
    if (E.cx == 0){
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar(){
    if (E.cy == E.numrows) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0){
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }

}

/*** file i/o ***/

char *editorRowsToString(int *buflen){
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++){
        totlen += E.row[j].size + 1; // 1 for newline character
    }
    *buflen = totlen; // we have total length of the string

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++){
        memcpy(p, E.row[j].chars, E.row[j].size); // copy contents of each row to end of buffer
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename); // allocates memory and assumes user frees it; duplicates string.

    
    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')){
            --linelen;
        }
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;


}


// editorSave() writes the actual string
void editorSave(){
    if (E.filename == NULL){
        E.filename = editorPrompt("Save as: %s (ESC To cancel)", NULL);
        if (E.filename == NULL){
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);
    // o_creat for creating new file if it doesn't exist and open for read and write o_rdwr
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // 0644 is the standard permission code
    
    if (fd != -1){
        if (ftruncate(fd, len) != -1){ // set file's size to length
            if (write(fd, buf, len) == len){ // this is a safe process
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        //editorInsertRow
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}


/*** find  ***/
// When the user types a search query and presses Enter, we’ll loop through all the rows of the file, and if a row contains their query string, we’ll move the cursor to the match.
void editorFindCallback(char *query, int key){
    static int last_match = -1;
    static int direction = 1;

    // save the original contents of hl in a static variable named saved_hl
    // restore hl to the contents of saved_hl at the top of the callback
    static int saved_hl_line;
    static char *saved_hl = NULL;
    if (saved_hl)
    {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b')
    {
        last_match = -1; // no match
        direction = 1; // go forwards
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;
    }
    else
    {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1)
        direction = 1;
    int current = last_match;

    int i;
    for (i = 0; i < E.numrows; i++)
    {
        current += direction;
        if (current == -1)
            current = E.numrows - 1;
        else if (current == E.numrows)
            current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query); // get query substring from render
        if (match)
        {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);

            // match - row->render is the index into render of the match, so we use that as our index into hl
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query)); // matched substring to HL_MATCH
            break;
        }
    }
}

void editorFind(){
    // we’ll have to save their cursor position and scroll position, and restore those values after the search is cancelled.
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if (query)
        free(query);
    else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}


/*** append buffer ***/
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0} // our buffer

void abAppend(struct abuf *ab, const char *s, int len){
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len); // we copy s
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}

/*** output ***/

void editorScroll(){
    E.rx = 0;
    if (E.cy < E.numrows){
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff){
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows){
        E.rowoff = E.cy - E.screenrows + 1;
    }

    if (E.rx < E.coloff){
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab){
    int y;
    for (y = 0; y < E.screenrows; y++){
        int filerow = y + E.rowoff; // correct range of lines of file
        if (filerow >= E.numrows){
            if (E.numrows == 0 && y == E.screenrows / 3) // for new file welcome message, text buffer empty
            {
                // welcome message
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Mim editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;
                // center message
                int padding = (E.screencols - welcomelen) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            // Color each digit character RED
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff]; // highlighter
            int current_color = -1;
            int j;
            for (j = 0; j < len; j++) // loop through characters!
            {
                if (iscntrl(c[j]))
                {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1)
                    {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                }
                else if (hl[j] == HL_NORMAL)
                {
                    if (current_color != -1)
                    {
                        abAppend(ab, "\x1b[39m", 5); // 39 sets default color
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else
                {  
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color)
                    {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color); // write escape sequences into a buffer, passed to abAppend
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5); // 39 sets default color
        }
    
        abAppend(ab, "\x1b[K", 3); // clear each line in redraw

        abAppend(ab, "\r\n", 2);
    }
}

// editorDrawStatusBar() displays with inverted colors and space characters
void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m", 4); // 7 stands for goinf to inverted colors
    char status[80], rstatus[80]; // notice below 20 characters tops
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");

    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                        E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows); // status bar

    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols){
        if (E.screencols - len == rlen){ // we stop printing spaces; bar printed
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3); // go back to normal formating
    abAppend(ab, "\r\n", 2); // make room for display message below
}

void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab, "\x1b[K", 3); // clears the bar
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5){ // message must be < 5 seconds old
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursors at beginning
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    //move cursor to position stored in E.cx and E.cy
    char buf[32];          /// specify exact position for cursor
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// editorSetStatusMessage() takes a format string and a variable number of arguments, like printf()
void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char*, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if (buflen != 0){
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b'){
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r'){
            if (buflen != 0){
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128){
            if (buflen == bufsize - 1){
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

// editorProcessKeypress() waits for keypress and handles it.
// move cursor with wasd
void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch(key){
        case ARROW_LEFT:
            if (E.cx != 0){
                E.cx--;
            } else if (E.cy > 0){
                E.cy--;
                E.cx = E.row[E.cy].size;
            }   
            break;
        case ARROW_RIGHT:
            // if (E.cx != E.screencols - 1){
            if (row && E.cx < row->size){
                E.cx++;
            } else if (row && E.cx == row->size){
                E.cy++;
                E.cx = 0;
            }
            // }
            break;
        case ARROW_UP:
            if (E.cy != 0){
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows){
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen){
        E.cx = rowlen;
    }
}

void editorProcessKeypress(){
    static int quit_times = KILO_QUIT_TIMES;
    int c = editorReadKey();

    switch(c){
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('w'): // it was q before
            if (E.dirty && quit_times > 0){
                editorSetStatusMessage("WARNING! File has unsaved changes. "
                "Press Ctrl-W %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;
        
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT); // equivalent to ->
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            { // code block allows to declare variable
                if (c == PAGE_UP) /// these 2 ifs allow us scroll entire pages!
                {
                    E.cy = E.rowoff;
                }
                else if (c == PAGE_DOWN)
                {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows)
                        E.cy = E.numrows;
                }
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
}


/*** init ***/

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0'; // no messgae by default
    E.statusmsg_time = 0; // will contain timestamp of status message
    E.syntax = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2; // so that editorDrawRows() doesn't try to draw
    // a line of text at the bottom of the screen.
}

int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if (argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-W = quit | Ctrl-F = find");

    while (1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}