/* See port_settings.h: writing a setting into the settings program's file. */
#include "port_settings.h"

#include "port_ui.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* ini_path() {
  static const char* path = std::getenv("MKGP2PC_INI");
  return path != nullptr && *path != 0 ? path : nullptr;
}

bool read_all(const char* path, std::string& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0) {
    out.append(buf, n);
  }
  std::fclose(f);
  return true;
}

/* The settings program's file is UTF-16 (little endian, with the mark):
 * the Windows profile functions keep it that way. Any other file is taken
 * as bytes that are ASCII where it matters. */
std::string utf16_to_utf8(const std::string& raw) {
  std::string out;
  for (size_t i = 2; i + 1 < raw.size(); i += 2) {
    unsigned c = static_cast<unsigned char>(raw[i]) | (static_cast<unsigned char>(raw[i + 1]) << 8);
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size()) {
      unsigned lo = static_cast<unsigned char>(raw[i + 2]) | (static_cast<unsigned char>(raw[i + 3]) << 8);
      if (lo >= 0xDC00 && lo < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 63));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    }
  }
  return out;
}

std::string utf8_to_utf16(const std::string& text) {
  std::string out("\xFF\xFE", 2);
  auto put = [&out](unsigned u) {
    out += static_cast<char>(u & 255);
    out += static_cast<char>(u >> 8);
  };
  const unsigned char* s = reinterpret_cast<const unsigned char*>(text.data());
  const unsigned char* end = s + text.size();
  while (s < end) {
    unsigned c = *s++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more != 0) {
      c &= 0x3Fu >> more;
    }
    for (; more > 0 && s < end; more--) {
      c = (c << 6) | (*s++ & 63);
    }
    if (c >= 0x10000) {
      c -= 0x10000;
      put(0xD800 + (c >> 10));
      put(0xDC00 + (c & 0x3FF));
    } else {
      put(c);
    }
  }
  return out;
}

std::string trimmed(const std::string& line) {
  size_t a = line.find_first_not_of(" \t\r");
  size_t b = line.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : line.substr(a, b - a + 1);
}

bool same_name(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    x = x >= 'A' && x <= 'Z' ? static_cast<char>(x + 32) : x;
    y = y >= 'A' && y <= 'Z' ? static_cast<char>(y + 32) : y;
    if (x != y) {
      return false;
    }
  }
  return true;
}

/* Sets key=value lines in one section of the file, like the Windows
 * profile functions do: a line that is there is replaced, another is added
 * at the section's end, a missing section is added at the file's end.
 * Everything else in the file stays as it is. */
bool put_lines(const char* section, const char* const* keys, const char* const* values, int count) {
  const char* path = ini_path();
  if (path == nullptr) {
    return false;
  }
  std::string raw;
  const bool exists = read_all(path, raw);
  const bool utf16 = !exists || raw.empty() || (raw.size() >= 2 && raw.compare(0, 2, "\xFF\xFE", 2) == 0);
  const std::string text = utf16 ? utf16_to_utf8(raw) : raw;
  std::vector<std::string> lines;
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
    at = nl == std::string::npos ? text.size() : nl + 1;
  }
  /* the section: [begin, end) are its lines after the heading */
  size_t begin = lines.size(), end = lines.size();
  for (size_t i = 0; i < lines.size(); i++) {
    const std::string t = trimmed(lines[i]);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      if (begin != lines.size()) {
        end = i;
        break;
      }
      if (same_name(t.substr(1, t.size() - 2), section)) {
        begin = i + 1;
      }
    }
  }
  if (begin == lines.size()) {
    bool found = false;
    for (const std::string& l : lines) {
      const std::string t = trimmed(l);
      found |= t.size() >= 2 && t.front() == '[' && t.back() == ']' && same_name(t.substr(1, t.size() - 2), section);
    }
    if (!found) {
      lines.push_back(std::string("[") + section + "]");
      begin = end = lines.size();
    }
  }
  for (int k = 0; k < count; k++) {
    const std::string line = std::string(keys[k]) + "=" + values[k];
    bool done = false;
    for (size_t i = begin; i < end && !done; i++) {
      size_t eq = lines[i].find('=');
      if (eq != std::string::npos && lines[i][0] != ';' && same_name(trimmed(lines[i].substr(0, eq)), keys[k])) {
        lines[i] = line;
        done = true;
      }
    }
    if (!done) {
      /* after the section's last line that is not empty */
      size_t at = end;
      while (at > begin && trimmed(lines[at - 1]).empty()) {
        at--;
      }
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
      end++;
    }
  }
  std::string out;
  for (const std::string& l : lines) {
    out += l;
    out += "\r\n";
  }
  if (utf16) {
    out = utf8_to_utf16(out);
  }
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "settings: cannot write %s\n", path);
    return false;
  }
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

std::vector<const Mkgp2pcSettingRow*>& extra_rows() {
  static std::vector<const Mkgp2pcSettingRow*> rows;
  return rows;
}

} // namespace

extern "C" {

int mkgp2pc_settings_can_save(void) { return ini_path() != nullptr; }

void mkgp2pc_settings_save_section(const char* section, const char* const* keys, const char* const* values, int count) {
  if (put_lines(section, keys, values, count)) {
    std::fprintf(stderr, "settings: [%s] %s=%s%s saved\n", section, keys[0], values[0], count > 1 ? " ..." : "");
  }
}

void mkgp2pc_settings_save(const char* key, const char* value) {
  mkgp2pc_settings_save_section("settings", &key, &value, 1);
}

void mkgp2pc_settings_save_int(const char* key, int value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%d", value);
  mkgp2pc_settings_save(key, text);
}

void mkgp2pc_settings_add_graphics(const Mkgp2pcSettingRow* row) {
  extra_rows().push_back(row);
  /* the menu's font holds only the letters announced before it is made */
  portui::announce(row->ja);
  for (int i = 0; i < row->count; i++) {
    portui::announce(row->choices_ja[i]);
  }
}

int mkgp2pc_settings_graphics_rows(const Mkgp2pcSettingRow* const** rows) {
  *rows = extra_rows().data();
  return static_cast<int>(extra_rows().size());
}

} // extern "C"
