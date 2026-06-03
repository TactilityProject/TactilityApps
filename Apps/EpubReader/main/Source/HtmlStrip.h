#pragma once

#include <string>

// Strips HTML/XHTML to a plain text + ESC-token string suitable for rendering.
//
// Output format:
//   - Every paragraph begins with an alignment token: ESC + 'L'|'C'|'R'
//   - Paragraphs are separated by '\n\n'
//   - Single '\n' = line break within a paragraph
//   - Bold/italic spans are bracketed by ESC tokens (for future span rendering):
//       ESC+'B' = bold on,  ESC+'b' = bold off
//       ESC+'I' = italic on, ESC+'i' = italic off
//   - All inline ESC tokens are 2 bytes (ESC + code); renderers that don't
//     support inline styles strip them before setting label text.
//
// HTML handling:
//   - <h1>/<h2> → center aligned; <h3>-<h6> → left aligned
//   - <p>, <div> with style="text-align: center/right" → center/right aligned
//   - <b>, <strong>, <span style="font-weight:bold"> → bold on/off tokens
//   - <i>, <em>, <span style="font-style:italic"> → italic on/off tokens
//   - <br> → '\n'
//   - <li> → '\n- ' prefix
//   - <style>, <script>, <head> blocks suppressed entirely
//   - All HTML entities decoded; multi-byte UTF-8 passed through
//   - &shy; → U+00AD soft hyphen (LVGL respects this for line breaking)
//   - Typographic entities (&mdash;, &lsquo; etc.) → proper Unicode
//   - Liang hyphenation applied to ASCII words (English, with soft hyphens)

void stripHtmlToText(const std::string& html, std::string& out);
