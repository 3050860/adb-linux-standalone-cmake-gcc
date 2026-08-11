#include "line_printer.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/time.h>
// Make sure fwrite is really adb_fwrite which works for UTF-8 on Windows.
#include <sysdeps.h>

// Stuff from ninja's util.h that's needed below.
#include <vector>
using namespace std;
// This does not account for multiple UTF-8 bytes corresponding to a single Unicode code point, or
// multiple code points corresponding to a single grapheme cluster (user-perceived character).
string ElideMiddle(const string& str, size_t width) {
  const int kMargin = 3;  // Space for "...".
  string result = str;
  if (result.size() + kMargin > width) {
    size_t elide_size = (width - kMargin) / 2;
    result = result.substr(0, elide_size)
      + "..."
      + result.substr(result.size() - elide_size, elide_size);
  }
  return result;
}

LinePrinter::LinePrinter() : have_blank_line_(true) {
  const char* term = getenv("TERM");
  smart_terminal_ = unix_isatty(STDERR_FILENO) && term && string(term) != "dumb";
}

static void Out(const std::string& s) {
  // Avoid fprintf and C strings, since the actual output might contain null
  // bytes like UTF-16 does (yuck).
  fwrite(s.data(), 1, s.size(), stderr);
}

void LinePrinter::Print(string to_print, LineType type) {
  if (quiet_ && type == LineType::INFO) {
    return;
  }

  if (!smart_terminal_) {
    if (type == LineType::INFO) {
        info_line_ = to_print + "\n";
    } else {
        Out(to_print + "\n");
    }
    return;
  }

  // Print over previous line, if any.
  // On Windows, calling a C library function writing to stdout also handles
  // pausing the executable when the "Pause" key or Ctrl-S is pressed.
  Out("\r");

  if (type == INFO) {
    // Limit output to width of the terminal if provided so we don't cause
    // line-wrapping.
    winsize size;
    if ((ioctl(0, TIOCGWINSZ, &size) == 0) && size.ws_col) {
      to_print = ElideMiddle(to_print, size.ws_col);
    }
    Out(to_print);
    Out("\x1B[K");  // Clear to end of line.


    have_blank_line_ = false;
  } else {
    Out(to_print);
    Out("\n");
    have_blank_line_ = true;
  }
}

void LinePrinter::KeepInfoLine() {
  if (smart_terminal_) {
      if (!have_blank_line_) Out("\n");
      have_blank_line_ = true;
  } else {
      Out(info_line_);
      info_line_.clear();
  }
}
