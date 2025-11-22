#include <ncurses.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "ui.h"

#define UI_PREF_WIDTH 80
#define UI_PREF_HEIGHT 20
#define UI_MIN_HEIGHT 12

static bool g_ui_ready = false;
static WINDOW *main_win = NULL;
static WINDOW *status_win = NULL;
static int layout_width = 0;
static int layout_height = 0;
static int cached_cols = 0;
static int cached_lines = 0;

static void ui_destroy_windows(void)
{
    if (status_win) {
        delwin(status_win);
        status_win = NULL;
    }
    if (main_win) {
        delwin(main_win);
        main_win = NULL;
    }
    layout_width = 0;
    layout_height = 0;
}

static int clamp_row(int row)
{
    if (layout_height <= 0) {
        return 0;
    }
    if (row < 0) {
        return 0;
    }
    if (row >= layout_height) {
        return layout_height - 1;
    }
    return row;
}

static int clamp_col(int col)
{
    if (layout_width <= 0) {
        return 0;
    }
    if (col < 0) {
        return 0;
    }
    if (col >= layout_width) {
        return layout_width - 1;
    }
    return col;
}

static void ui_relayout(void)
{
    if (!g_ui_ready) {
        return;
    }

    int term_cols = COLS;
    int term_lines = LINES;

    if (main_win && term_cols == cached_cols && term_lines == cached_lines) {
        return;
    }

    cached_cols = term_cols;
    cached_lines = term_lines;

    ui_destroy_windows();

    if (term_cols <= 0 || term_lines <= 0) {
        return;
    }

    int status_rows = (term_lines > 4) ? 1 : 0;
    int max_main_height = term_lines - status_rows;
    if (max_main_height <= 0) {
        status_rows = 0;
        max_main_height = term_lines;
    }

    int height = max_main_height;
    if (height > UI_PREF_HEIGHT) {
        height = UI_PREF_HEIGHT;
    }
    if (height < UI_MIN_HEIGHT && max_main_height >= UI_MIN_HEIGHT) {
        height = UI_MIN_HEIGHT;
    }
    if (height <= 0) {
        height = max_main_height;
    }

    int width = term_cols;
    if (width > UI_PREF_WIDTH) {
        width = UI_PREF_WIDTH;
    }
    if (width <= 0) {
        return;
    }

    int block_height = height + status_rows;
    if (block_height > term_lines) {
        block_height = term_lines;
    }

    int starty = (term_lines - block_height) / 2;
    if (starty < 0) {
        starty = 0;
    }
    int startx = (term_cols - width) / 2;
    if (startx < 0) {
        startx = 0;
    }

    main_win = newwin(height, width, starty, startx);
    if (!main_win) {
        return;
    }
    keypad(main_win, TRUE);
    layout_height = height;
    layout_width = width;

    if (status_rows > 0) {
        int status_y = starty + height;
        if (status_y < term_lines) {
            status_win = newwin(1, width, status_y, startx);
        }
    }

    if (main_win) {
        werase(main_win);
        wrefresh(main_win);
    }
    if (status_win) {
        werase(status_win);
        wrefresh(status_win);
    }
    erase();
    refresh();
}

static bool ui_layout_ready(void)
{
    if (!g_ui_ready) {
        return false;
    }
    ui_relayout();
    return main_win != NULL;
}

static bool ui_begin_frame(void)
{
    if (!ui_layout_ready()) {
        return false;
    }
    werase(main_win);
    return true;
}

static void draw_header(const char *title, const char *subtitle)
{
    if (!main_win || layout_width <= 0) {
        return;
    }

    const char *header = title ? title : INSTALLER_NAME;
    int padding = (layout_width > 4) ? 2 : 0;

    int title_row = clamp_row(0);
    mvwhline(main_win, title_row, 0, ' ', layout_width);
    mvwprintw(main_win, title_row, padding, "%s", header);

    if (layout_height > 1) {
        int subtitle_row = clamp_row(1);
        mvwhline(main_win, subtitle_row, 0, ' ', layout_width);
        if (subtitle) {
            mvwprintw(main_win, subtitle_row, padding, "%s", subtitle);
        }
    }

    if (layout_height > 2) {
        int line_row = clamp_row(2);
        mvwhline(main_win, line_row, 0, ACS_HLINE, layout_width);
    }
}

static void draw_progress_bar(int row, int col, int width, int percent)
{
    if (!main_win || width < 4) {
        return;
    }

    int inner = width - 2;
    if (inner <= 0) {
        return;
    }
    int filled = (percent * inner) / 100;
    mvwaddch(main_win, row, col, '[');
    for (int i = 0; i < inner; ++i) {
        waddch(main_win, (i < filled) ? '=' : ' ');
    }
    waddch(main_win, ']');
}

static void draw_loading_frame(const char *title, const char *message, int percent, char spinner)
{
    if (!ui_begin_frame()) {
        return;
    }

    draw_header(title ? title : INSTALLER_NAME, NULL);

    int msg_row = clamp_row(4);
    mvwprintw(main_win, msg_row, clamp_col(2), "%s", message ? message : "Working...");

    int bar_row = clamp_row(msg_row + 2);
    int bar_col = clamp_col(2);
    int bar_width = layout_width - 4;
    int max_available = layout_width - bar_col - 1;
    if (bar_width > max_available) {
        bar_width = max_available;
    }
    if (bar_width < 8) {
        bar_width = max_available;
    }
    if (bar_width < 4) {
        wrefresh(main_win);
        return;
    }

    wattron(main_win, COLOR_PAIR(1));
    draw_progress_bar(bar_row, bar_col, bar_width, percent);
    wattroff(main_win, COLOR_PAIR(1));

    mvwprintw(main_win,
              clamp_row(bar_row + 1),
              bar_col,
              "%3d%% %c",
              percent,
              spinner);

    wrefresh(main_win);
}

static int wait_for_keypress(void)
{
    WINDOW *win = main_win ? main_win : stdscr;
    if (!win) {
        return ERR;
    }

    int ch;
    while ((ch = wgetch(win)) != ERR) {
        if (ch == KEY_RESIZE) {
            ui_relayout();
            return KEY_RESIZE;
        }
        if (ch == '\n' || ch == KEY_ENTER || ch == 27 || ch == 'q') {
            return ch;
        }
    }
    return ch;
}

int ui_init(void)
{
    if (g_ui_ready) {
        return 0;
    }

    if (!initscr()) {
        fprintf(stderr, "Unable to initialize ncurses\n");
        return -1;
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_YELLOW, -1);
    init_pair(3, COLOR_RED, -1);

    g_ui_ready = true;
    ui_relayout();
    if (!main_win) {
        ui_shutdown();
        fprintf(stderr, "Unable to create installer window\n");
        return -1;
    }

    return 0;
}

void ui_shutdown(void)
{
    if (!g_ui_ready) {
        return;
    }
    ui_destroy_windows();
    cached_cols = 0;
    cached_lines = 0;
    endwin();
    g_ui_ready = false;
}

void ui_status(const char *message)
{
    if (!g_ui_ready) {
        return;
    }
    ui_relayout();
    if (!status_win) {
        return;
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(status_win, rows, cols);
    (void)rows;
    if (cols <= 0) {
        return;
    }
    int col = (cols > 2) ? 1 : 0;

    werase(status_win);
    mvwprintw(status_win, 0, col, "%s", message ? message : "");
    wrefresh(status_win);
}

static void show_modal_message(const char *title, const char *message, int color_pair)
{
    if (!g_ui_ready) {
        FILE *out = (color_pair > 0) ? stderr : stdout;
        if (title && message) {
            fprintf(out, "%s: %s\n", title, message);
        } else if (message) {
            fprintf(out, "%s\n", message);
        } else if (title) {
            fprintf(out, "%s\n", title);
        }
        return;
    }

    while (1) {
        if (!ui_begin_frame()) {
            FILE *out = (color_pair > 0) ? stderr : stdout;
            if (message) {
                fprintf(out, "%s\n", message);
            }
            return;
        }
        draw_header(title ? title : INSTALLER_NAME, NULL);
        int message_row = clamp_row(4);
        if (color_pair > 0) {
            wattron(main_win, COLOR_PAIR(color_pair) | A_BOLD);
        }
        mvwprintw(main_win, message_row, clamp_col(2), "%s", message ? message : "");
        if (color_pair > 0) {
            wattroff(main_win, COLOR_PAIR(color_pair) | A_BOLD);
        }

        int prompt_row = clamp_row(layout_height - 2);
        if (prompt_row <= message_row) {
            prompt_row = clamp_row(message_row + 1);
        }
        mvwprintw(main_win, prompt_row, clamp_col(2), "Press Enter to continue...");

        wrefresh(main_win);

        int key = wait_for_keypress();
        if (key == KEY_RESIZE || key == ERR) {
            continue;
        }
        break;
    }
}

void ui_message(const char *title, const char *message)
{
    show_modal_message(title, message, 0);
}

void ui_error(const char *title, const char *message)
{
    show_modal_message(title ? title : "Error", message, 3);
}

int ui_run_shell_command(const char *title, const char *command)
{
    if (!command || !command[0]) {
        return -1;
    }

    if (!g_ui_ready) {
        int rc = system(command);
        if (rc == -1) {
            return -1;
        }
        if (WIFEXITED(rc)) {
            return WEXITSTATUS(rc);
        }
        return -1;
    }

    def_prog_mode();
    endwin();
    if (title && title[0]) {
        fprintf(stdout, "\n=== %s ===\n", title);
    }
    fprintf(stdout, "(Ctrl+C to interrupt. After fdisk exits, press Enter to return to the installer.)\n\n");
    fflush(stdout);
    int rc = system(command);
    fprintf(stdout, "\nCommand finished. Press Enter to continue...");
    fflush(stdout);
    while (1) {
        int ch = getchar();
        if (ch == '\n' || ch == EOF) {
            break;
        }
    }
    reset_prog_mode();
    refresh();
    ui_relayout();

    if (rc == -1) {
        return -1;
    }
    if (WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
    return -1;
}

int ui_wait_for_process(const char *title, const char *message, pid_t pid)
{
    if (pid <= 0) {
        return -1;
    }

    int status = 0;
    int percent = 0;
    const char spinner[] = "|/-\\";
    int spinner_idx = 0;
    bool interactive = ui_layout_ready();
    struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 120 * 1000000};
    int wait_flags = interactive ? WNOHANG : 0;

    while (1) {
        pid_t res = waitpid(pid, &status, wait_flags);
        if (res == 0) {
            percent = (percent + 3);
            if (percent > 92) {
                percent = 65 + (percent % 30);
            }
            draw_loading_frame(title, message, percent, spinner[spinner_idx++ % 4]);
            nanosleep(&sleep_time, NULL);
            continue;
        }
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        break;
    }

    if (interactive) {
        draw_loading_frame(title, message, 100, ' ');
        napms(120);
    }

    return status;
}

bool ui_confirm(const char *title, const char *message)
{
    const char *items[] = {"Yes", "No"};
    int choice = ui_menu(title, message, items, 2, 1);
    return choice == 0;
}

int ui_menu(const char *title,
            const char *subtitle,
            const char **items,
            size_t count,
            int selected)
{
    if (!g_ui_ready || !items || count == 0) {
        return -1;
    }

    int highlight = selected;
    if (highlight < 0 || (size_t)highlight >= count) {
        highlight = 0;
    }

    while (1) {
        if (!ui_begin_frame()) {
            return -1;
        }
        draw_header(title ? title : INSTALLER_NAME, subtitle);
        int text_col = (layout_width > 8) ? 4 : 0;
        for (size_t i = 0; i < count; ++i) {
            int row = clamp_row(4 + (int)i);
            if ((int)i == highlight) {
                wattron(main_win, A_REVERSE | COLOR_PAIR(1));
                mvwprintw(main_win, row, text_col, "> %s", items[i]);
                wattroff(main_win, A_REVERSE | COLOR_PAIR(1));
            } else {
                mvwprintw(main_win, row, text_col, "  %s", items[i]);
            }
        }
        mvwprintw(main_win,
                  clamp_row(layout_height - 2),
                  clamp_col(2),
                  "Use arrow keys to navigate, Enter to select, q to exit");
        wrefresh(main_win);

        int ch = wgetch(main_win);
        if (ch == ERR) {
            continue;
        }
        if (ch == KEY_RESIZE) {
            ui_relayout();
            continue;
        }
        switch (ch) {
        case KEY_UP:
            highlight = (highlight == 0) ? (int)count - 1 : highlight - 1;
            break;
        case KEY_DOWN:
            highlight = (highlight == (int)count - 1) ? 0 : highlight + 1;
            break;
        case 'q':
        case 27:
            return -1;
        case '\n':
        case KEY_ENTER:
            return highlight;
        default:
            break;
        }
    }

    return -1;
}

int ui_prompt_input(const char *title,
                    const char *prompt,
                    char *buffer,
                    size_t buffer_len,
                    const char *initial,
                    bool secret)
{
    if (!buffer || buffer_len == 0) {
        return -1;
    }

    char temp[1024];
    memset(temp, 0, sizeof(temp));
    size_t limit = 0;
    if (buffer_len > 0) {
        limit = buffer_len - 1;
    }
    if (limit > sizeof(temp) - 1) {
        limit = sizeof(temp) - 1;
    }

    size_t len = 0;
    if (initial && *initial) {
        snprintf(temp, sizeof(temp), "%.*s", (int)limit, initial);
        len = strlen(temp);
    } else {
        temp[0] = '\0';
    }

    while (1) {
        if (!ui_begin_frame()) {
            return -1;
        }
        draw_header(title ? title : INSTALLER_NAME, NULL);
        mvwprintw(main_win, clamp_row(4), clamp_col(2), "%s", prompt ? prompt : "Input:");
        int input_row = clamp_row(6);
        int input_col = clamp_col(4);
        if (secret) {
            char mask[1024];
            size_t mask_len = len < sizeof(mask) - 1 ? len : sizeof(mask) - 1;
            memset(mask, '*', mask_len);
            mask[mask_len] = '\0';
            mvwprintw(main_win, input_row, input_col, "%s", mask);
        } else {
            mvwprintw(main_win, input_row, input_col, "%s", temp);
        }
        int cursor_col = clamp_col(input_col + (int)len);
        wmove(main_win, input_row, cursor_col);
        wrefresh(main_win);

        int ch = wgetch(main_win);
        if (ch == ERR) {
            continue;
        }
        if (ch == KEY_RESIZE) {
            ui_relayout();
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            snprintf(buffer, buffer_len, "%s", temp);
            return 0;
        }
        if (ch == 27) {
            return -1;
        }
        if (ch == KEY_BACKSPACE || ch == 127) {
            if (len > 0) {
                temp[--len] = '\0';
            }
            continue;
        }

        if (isprint(ch) && len < limit) {
            temp[len++] = (char)ch;
            temp[len] = '\0';
        }
    }
}
