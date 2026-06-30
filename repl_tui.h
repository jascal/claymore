// repl_tui.h — a tiny, dependency-free terminal UI for the REPL: a fixed input line + a status footer anchored at the
// bottom while answer output scrolls ABOVE them (a DECSTBM scroll region). Header-only; an IDENTICAL copy ships in
// claymore and sgiandubh (no shared lib) — keep the two in sync.
//
// It engages ONLY on an interactive TTY (and not when TERM=dumb, or the opt-out env var is set, or --plain). Otherwise
// readline() degrades to a plain std::getline so pipes, scripts and tests are byte-for-byte unchanged. The terminal is
// restored on stop(), on normal exit (atexit), and on SIGTERM/SIGHUP.
//
// Model: the screen splits into [scroll region: rows 1..H-2] + [input line: row H-1] + [footer: row H]. The host program
// keeps using ordinary printf/fprintf for its output — after each submitted line the cursor is parked at the bottom of
// the scroll region, so that output naturally scrolls there and never touches the input/footer rows. The host only has
// to (1) call start()/stop() around the loop, (2) set_footer() when state changes, (3) read input via readline().
#pragma once
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace repltui {

inline volatile sig_atomic_t g_resized = 0;
inline void on_winch(int) { g_resized = 1; }

class Repl {
public:
    enum Read { LINE, EOF_QUIT };

    // Engage the TUI if stdin/stdout are an interactive terminal. Returns true if engaged; false → plain fallback mode
    // (readline() still works, just as a getline). `disable_env`: if that env var is set, stay in plain mode.
    bool start(const char* disable_env = "") {
        disable_env_ = disable_env;
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return (active_ = false);
        const char* term = getenv("TERM");
        if (term && std::string(term) == "dumb") return (active_ = false);
        if (disable_env_ && *disable_env_ && getenv(disable_env_)) return (active_ = false);
        if (tcgetattr(STDIN_FILENO, &orig_) != 0) return (active_ = false);
        termios raw = orig_;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);    // char-at-a-time, no echo; Ctrl-C/Z arrive as bytes (we handle them)
        raw.c_iflag &= ~(IXON | ICRNL);            // no XON/XOFF; Enter arrives as '\r' (we accept '\r' and '\n')
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;   // OPOST left ON so the host's "\n" still does CR+LF in the region
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return (active_ = false);
        self_ = this;
        std::atexit(&Repl::atexit_restore);
        signal(SIGWINCH, on_winch);
        signal(SIGTERM, &Repl::on_fatal);
        signal(SIGHUP, &Repl::on_fatal);
        querysize();
        set_region();
        return (active_ = true);
    }

    void stop() {
        if (!active_) return;
        active_ = false;
        std::string s = "\x1b[r";                                  // release the scroll region (whole screen again)
        s += "\x1b[" + std::to_string(rows_) + ";1H";              // park at the bottom
        s += "\x1b[?25h\n";                                        // show cursor + a fresh line for the shell prompt
        wr(s);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_);
        self_ = nullptr;
    }

    void set_footer(const std::string& s) { footer_ = s; }        // drawn by the next readline()

    Read readline(const std::string& prompt, std::string& out) {
        if (!active_) {                                            // plain fallback — identical to the old REPL
            fputs(prompt.c_str(), stderr); fflush(stderr);
            if (!std::getline(std::cin, out)) return EOF_QUIT;
            return LINE;
        }
        prompt_ = prompt; buf_.clear(); cur_ = 0; hist_pos_ = (int)history_.size();
        draw_footer(); draw_input();
        for (;;) {
            if (g_resized) { g_resized = 0; querysize(); set_region(); draw_footer(); draw_input(); }
            char c;
            ssize_t n = ::read(STDIN_FILENO, &c, 1);
            if (n < 0) { if (errno == EINTR) continue; return EOF_QUIT; }
            if (n == 0) return EOF_QUIT;
            if (c == '\r' || c == '\n') { commit(); out = buf_; if (!buf_.empty()) push_history(buf_); return LINE; }
            else if (c == 4)   { if (buf_.empty()) { echo_park(); return EOF_QUIT; } del_under(); }   // Ctrl-D
            else if (c == 3)   { buf_.clear(); cur_ = 0; draw_input(); }                              // Ctrl-C: clear line
            else if (c == 127 || c == 8) backspace();
            else if (c == 21)  { buf_.erase(0, cur_); cur_ = 0; draw_input(); }                       // Ctrl-U: kill to start
            else if (c == 11)  { buf_.erase(cur_); draw_input(); }                                    // Ctrl-K: kill to end
            else if (c == 1)   { cur_ = 0; draw_input(); }                                            // Ctrl-A: home
            else if (c == 5)   { cur_ = buf_.size(); draw_input(); }                                  // Ctrl-E: end
            else if (c == 12)  { redraw_all(); }                                                      // Ctrl-L: redraw
            else if (c == 27)  handle_escape();
            else if ((unsigned char)c >= 32) { buf_.insert(buf_.begin() + cur_, c); cur_++; draw_input(); }
        }
    }

    bool active() const { return active_; }

private:
    bool active_ = false;
    termios orig_{};
    int rows_ = 24, cols_ = 80;
    std::string footer_, prompt_, buf_;
    size_t cur_ = 0;
    std::vector<std::string> history_;
    int hist_pos_ = 0;
    const char* disable_env_ = "";

    static Repl* self_;
    static void atexit_restore() { if (self_) self_->stop(); }
    static void on_fatal(int sig) { if (self_) self_->stop(); signal(sig, SIG_DFL); raise(sig); }

    void wr(const std::string& s) { ssize_t r = ::write(STDOUT_FILENO, s.data(), s.size()); (void)r; }

    void querysize() {
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 0) {
            rows_ = ws.ws_row; cols_ = ws.ws_col;
        } else { rows_ = 24; cols_ = 80; }
    }
    void set_region() {                                            // scroll region = rows 1..H-2, cursor parked at its bottom
        wr("\x1b[1;" + std::to_string(rows_ - 2) + "r");
        wr("\x1b[" + std::to_string(rows_ - 2) + ";1H");
    }
    void redraw_all() { wr("\x1b[2J"); set_region(); draw_footer(); draw_input(); }

    void draw_footer() {
        std::string f = footer_;
        if ((int)f.size() > cols_) f = f.substr(0, cols_);
        // row H, clear, dim — a thin status bar under the input line.
        wr("\x1b[" + std::to_string(rows_) + ";1H\x1b[2K\x1b[2m" + f + "\x1b[22m");
        place_cursor();
    }
    void draw_input() {
        int avail = cols_ - (int)prompt_.size() - 1; if (avail < 1) avail = 1;
        size_t start = 0;                                         // horizontal scroll so the cursor stays visible
        if ((int)cur_ >= avail) start = cur_ - avail + 1;
        std::string vis = buf_.substr(start, avail);
        wr("\x1b[" + std::to_string(rows_ - 1) + ";1H\x1b[2K" + prompt_ + vis);
        last_start_ = start;
        place_cursor();
    }
    void place_cursor() {
        size_t col = prompt_.size() + (cur_ - last_start_) + 1;
        wr("\x1b[" + std::to_string(rows_ - 1) + ";" + std::to_string(col) + "H");
    }
    void backspace() { if (cur_ > 0) { buf_.erase(buf_.begin() + (cur_ - 1)); cur_--; draw_input(); } }
    void del_under() { if (cur_ < buf_.size()) { buf_.erase(buf_.begin() + cur_); draw_input(); } }

    void handle_escape() {                                        // ESC [ X  or  ESC O X  — arrows / home / end / delete
        char a, b;
        if (::read(STDIN_FILENO, &a, 1) != 1) return;
        if (a != '[' && a != 'O') return;
        if (::read(STDIN_FILENO, &b, 1) != 1) return;
        switch (b) {
            case 'A': hist_prev(); break;                         // up
            case 'B': hist_next(); break;                         // down
            case 'C': if (cur_ < buf_.size()) { cur_++; draw_input(); } break;  // right
            case 'D': if (cur_ > 0) { cur_--; draw_input(); } break;            // left
            case 'H': cur_ = 0; draw_input(); break;                            // home
            case 'F': cur_ = buf_.size(); draw_input(); break;                  // end
            case '1': case '3': case '4': case '7': case '8': {                 // ESC[N~ forms — swallow the trailing '~'
                char t; if (::read(STDIN_FILENO, &t, 1) == 1 && t == '~') {
                    if (b == '3') del_under();
                    else if (b == '1' || b == '7') { cur_ = 0; draw_input(); }
                    else if (b == '4' || b == '8') { cur_ = buf_.size(); draw_input(); }
                }
                break;
            }
            default: break;
        }
    }

    void hist_prev() {
        if (history_.empty() || hist_pos_ == 0) return;
        hist_pos_--; buf_ = history_[hist_pos_]; cur_ = buf_.size(); draw_input();
    }
    void hist_next() {
        if (hist_pos_ >= (int)history_.size()) return;
        hist_pos_++;
        buf_ = (hist_pos_ == (int)history_.size()) ? std::string() : history_[hist_pos_];
        cur_ = buf_.size(); draw_input();
    }
    void push_history(const std::string& s) {
        if (history_.empty() || history_.back() != s) history_.push_back(s);
    }

    // Echo the submitted line into the scroll region (so it joins the scrollback), and park the cursor at the bottom of
    // the region so the host's subsequent output scrolls there — never onto the input/footer rows.
    void commit() {
        wr("\x1b[" + std::to_string(rows_ - 2) + ";1H");          // bottom of the scroll region
        wr("\x1b[2K" + prompt_ + buf_ + "\r\n");                  // \n at the region bottom scrolls the region up by one
    }
    void echo_park() { wr("\x1b[" + std::to_string(rows_ - 2) + ";1H\r\n"); }   // Ctrl-D on empty: just park + newline

    size_t last_start_ = 0;
};

inline Repl* Repl::self_ = nullptr;

}  // namespace repltui
