// md_render.h — dependency-free terminal renderer for Markdown + inline TeX / MathML math.
//
// Header-only, no external deps. An IDENTICAL copy ships in claymore and sgiandubh (separate repos, no shared lib) —
// keep them in sync. mdterm::render(text, color):
//   color=false → clean plaintext: math converted to Unicode, markdown markup stripped (for pipes / non-TTY).
//   color=true  → adds ANSI styling for a TTY (bold/italic/headings/code, math in cyan).
// Best-effort by design: anything it doesn't recognize degrades to readable text rather than erroring.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cctype>

namespace mdterm {

// ---- TeX → Unicode -------------------------------------------------------------------------------------------------
inline const std::map<std::string, std::string>& tex_symbols() {
    static const std::map<std::string, std::string> M = {
        {"alpha","α"},{"beta","β"},{"gamma","γ"},{"delta","δ"},{"epsilon","ε"},{"varepsilon","ε"},{"zeta","ζ"},
        {"eta","η"},{"theta","θ"},{"vartheta","ϑ"},{"iota","ι"},{"kappa","κ"},{"lambda","λ"},{"mu","μ"},{"nu","ν"},
        {"xi","ξ"},{"pi","π"},{"varpi","ϖ"},{"rho","ρ"},{"sigma","σ"},{"tau","τ"},{"upsilon","υ"},{"phi","φ"},
        {"varphi","φ"},{"chi","χ"},{"psi","ψ"},{"omega","ω"},{"Gamma","Γ"},{"Delta","Δ"},{"Theta","Θ"},{"Lambda","Λ"},
        {"Xi","Ξ"},{"Pi","Π"},{"Sigma","Σ"},{"Upsilon","Υ"},{"Phi","Φ"},{"Psi","Ψ"},{"Omega","Ω"},
        {"times","×"},{"div","÷"},{"cdot","·"},{"pm","±"},{"mp","∓"},{"ast","∗"},{"star","⋆"},{"circ","∘"},
        {"leq","≤"},{"le","≤"},{"geq","≥"},{"ge","≥"},{"neq","≠"},{"ne","≠"},{"approx","≈"},{"equiv","≡"},
        {"sim","∼"},{"simeq","≃"},{"cong","≅"},{"propto","∝"},{"ll","≪"},{"gg","≫"},{"doteq","≐"},
        {"rightarrow","→"},{"to","→"},{"longrightarrow","⟶"},{"leftarrow","←"},{"Rightarrow","⇒"},{"Leftarrow","⇐"},
        {"leftrightarrow","↔"},{"Leftrightarrow","⇔"},{"mapsto","↦"},{"implies","⇒"},{"iff","⇔"},{"uparrow","↑"},
        {"downarrow","↓"},{"infty","∞"},{"partial","∂"},{"nabla","∇"},{"forall","∀"},{"exists","∃"},{"nexists","∄"},
        {"in","∈"},{"notin","∉"},{"ni","∋"},{"subset","⊂"},{"subseteq","⊆"},{"supset","⊃"},{"supseteq","⊇"},
        {"cup","∪"},{"cap","∩"},{"emptyset","∅"},{"varnothing","∅"},{"setminus","∖"},{"sum","∑"},{"prod","∏"},
        {"coprod","∐"},{"int","∫"},{"oint","∮"},{"iint","∬"},{"sqrt","√"},{"land","∧"},{"wedge","∧"},{"lor","∨"},
        {"vee","∨"},{"lnot","¬"},{"neg","¬"},{"oplus","⊕"},{"otimes","⊗"},{"odot","⊙"},{"top","⊤"},{"bot","⊥"},
        {"vdash","⊢"},{"dashv","⊣"},{"models","⊨"},{"therefore","∴"},{"because","∵"},{"angle","∠"},{"perp","⊥"},
        {"parallel","∥"},{"mid","∣"},{"cdots","⋯"},{"ldots","…"},{"dots","…"},{"vdots","⋮"},{"ddots","⋱"},
        {"hbar","ℏ"},{"ell","ℓ"},{"Re","ℜ"},{"Im","ℑ"},{"aleph","ℵ"},{"wp","℘"},{"deg","°"},{"prime","′"},
        {"langle","⟨"},{"rangle","⟩"},{"lceil","⌈"},{"rceil","⌉"},{"lfloor","⌊"},{"rfloor","⌋"},{"cong","≅"},
        {"subsetneq","⊊"},{"supsetneq","⊋"},{"triangleq","≜"},{"mapsfrom","↤"},{"hookrightarrow","↪"},
        {"mathbb{R}","ℝ"},{"mathbb{Z}","ℤ"},{"mathbb{N}","ℕ"},{"mathbb{Q}","ℚ"},{"mathbb{C}","ℂ"},{"mathbb{H}","ℍ"},
        {"mathbb{P}","ℙ"},{"mathbb{E}","𝔼"},{"mathbb{F}","𝔽"},
    };
    return M;
}
inline const std::map<char, std::string>& superscripts() {
    static const std::map<char, std::string> M = {
        {'0',"⁰"},{'1',"¹"},{'2',"²"},{'3',"³"},{'4',"⁴"},{'5',"⁵"},{'6',"⁶"},{'7',"⁷"},{'8',"⁸"},{'9',"⁹"},
        {'+',"⁺"},{'-',"⁻"},{'=',"⁼"},{'(',"⁽"},{')',"⁾"},{'n',"ⁿ"},{'i',"ⁱ"},{'a',"ᵃ"},{'b',"ᵇ"},{'c',"ᶜ"},
        {'k',"ᵏ"},{'m',"ᵐ"},{'p',"ᵖ"},{'x',"ˣ"},{'y',"ʸ"},{'T',"ᵀ"},
    };
    return M;
}
inline const std::map<char, std::string>& subscripts() {
    static const std::map<char, std::string> M = {
        {'0',"₀"},{'1',"₁"},{'2',"₂"},{'3',"₃"},{'4',"₄"},{'5',"₅"},{'6',"₆"},{'7',"₇"},{'8',"₈"},{'9',"₉"},
        {'+',"₊"},{'-',"₋"},{'=',"₌"},{'(',"₍"},{')',"₎"},{'a',"ₐ"},{'e',"ₑ"},{'i',"ᵢ"},{'j',"ⱼ"},{'o',"ₒ"},
        {'x',"ₓ"},{'n',"ₙ"},{'m',"ₘ"},{'t',"ₜ"},{'k',"ₖ"},{'l',"ₗ"},{'p',"ₚ"},{'s',"ₛ"},
    };
    return M;
}

// read a {group} or a single token (a char, or a \command) at s[i]; advances i past it, returns the contents.
inline std::string read_group(const std::string& s, size_t& i) {
    if (i < s.size() && s[i] == '{') {
        int depth = 0; size_t start = ++i;
        for (; i < s.size(); ++i) {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') { if (depth == 0) break; depth--; }
        }
        std::string inner = s.substr(start, i - start);
        if (i < s.size()) ++i;                                   // consume '}'
        return inner;
    }
    if (i < s.size() && s[i] == '\\') {                          // a command token like \alpha
        size_t start = i++;
        while (i < s.size() && std::isalpha((unsigned char)s[i])) i++;
        return s.substr(start, i - start);
    }
    if (i < s.size()) return std::string(1, s[i++]);
    return "";
}

inline std::string tex_to_unicode(const std::string& in);   // fwd

// map each char of g to a script glyph; "" if any char has no mapping (caller falls back to ^(..)/_(..)).
inline std::string map_script(const std::string& g, const std::map<char, std::string>& tbl) {
    std::string out;
    for (char c : g) { auto it = tbl.find(c); if (it == tbl.end()) return ""; out += it->second; }
    return out;
}

inline std::string tex_to_unicode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        char c = in[i];
        if (c == '\\') {
            size_t j = i + 1; std::string cmd;
            if (j < in.size() && std::isalpha((unsigned char)in[j]))
                while (j < in.size() && std::isalpha((unsigned char)in[j])) cmd += in[j++];
            else if (j < in.size()) cmd = std::string(1, in[j++]);   // \, \! \( etc.
            if (cmd == "left" || cmd == "right" || cmd == "displaystyle" || cmd == "textstyle" ||
                cmd == "limits" || cmd == "nolimits") { i = j; continue; }
            if (cmd == "," || cmd == ";" || cmd == ":" || cmd == "!" || cmd == " " ||
                cmd == "quad" || cmd == "qquad") { out += ' '; i = j; continue; }
            if (cmd == "frac" || cmd == "dfrac" || cmd == "tfrac") {
                size_t k = j; std::string a = read_group(in, k), b = read_group(in, k);
                out += "(" + tex_to_unicode(a) + ")/(" + tex_to_unicode(b) + ")"; i = k; continue;
            }
            if (cmd == "sqrt") {
                size_t k = j;
                if (k < in.size() && in[k] == '[') { size_t e = in.find(']', k); if (e != std::string::npos) k = e + 1; }
                std::string a = read_group(in, k);
                out += "√(" + tex_to_unicode(a) + ")"; i = k; continue;
            }
            if (cmd == "mathbb" || cmd == "mathcal" || cmd == "mathrm" || cmd == "mathbf" || cmd == "mathsf" ||
                cmd == "mathit" || cmd == "text" || cmd == "textbf" || cmd == "operatorname") {
                size_t k = j; std::string a = read_group(in, k);
                if (cmd == "mathbb") {
                    auto it = tex_symbols().find("mathbb{" + a + "}");
                    if (it != tex_symbols().end()) { out += it->second; i = k; continue; }
                }
                out += tex_to_unicode(a); i = k; continue;
            }
            if (cmd == "hat" || cmd == "bar" || cmd == "vec" || cmd == "tilde" || cmd == "dot" || cmd == "overline") {
                size_t k = j; std::string a = read_group(in, k);
                out += tex_to_unicode(a); i = k; continue;       // drop the accent, keep the symbol
            }
            auto it = tex_symbols().find(cmd);
            if (it != tex_symbols().end()) { out += it->second; i = j; continue; }
            out += cmd;                                          // unknown command → keep its name (drop the backslash)
            i = j; continue;
        }
        if (c == '^' || c == '_') {
            size_t k = i + 1; std::string g = tex_to_unicode(read_group(in, k));
            const auto& tbl = (c == '^') ? superscripts() : subscripts();
            std::string mapped = map_script(g, tbl);
            out += !mapped.empty() ? mapped : (std::string(c == '^' ? "^(" : "_(") + g + ")");
            i = k; continue;
        }
        if (c == '{' || c == '}') { i++; continue; }             // strip stray braces
        out += c; i++;
    }
    std::string z; bool sp = false;                             // collapse runs of spaces
    for (char c : out) { if (c == ' ') { if (!sp) z += ' '; sp = true; } else { z += c; sp = false; } }
    return z;
}

// MathML → text: extract the text content of <math>…</math> (and any presentation tags), dropping the tags. Coarse but
// turns "<math><mi>x</mi><mo>+</mo><mn>1</mn></math>" into "x+1".
inline std::string strip_mathml(const std::string& s) {
    std::string lower; for (char c : s) lower += (char)std::tolower((unsigned char)c);
    if (lower.find("<math") == std::string::npos) return s;
    std::string out; size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            size_t e = s.find('>', i);
            if (e == std::string::npos) { out += s.substr(i); break; }
            i = e + 1; continue;                                // drop the tag
        }
        if (s[i] == '&') {                                      // a few common entities
            if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; continue; }
            if (s.compare(i, 4, "&lt;") == 0)  { out += '<'; i += 4; continue; }
            if (s.compare(i, 4, "&gt;") == 0)  { out += '>'; i += 4; continue; }
        }
        out += s[i++];
    }
    return out;
}

// ---- ANSI styling --------------------------------------------------------------------------------------------------
struct Style {
    bool color;
    std::string wrap(const char* on, const char* off, const std::string& s) const {
        return color ? (std::string(on) + s + off) : s;
    }
    std::string b(const std::string& s)    const { return wrap("\x1b[1m", "\x1b[22m", s); }   // bold
    std::string it(const std::string& s)   const { return wrap("\x1b[3m", "\x1b[23m", s); }   // italic
    std::string ul(const std::string& s)   const { return wrap("\x1b[4m", "\x1b[24m", s); }   // underline
    std::string dim(const std::string& s)  const { return wrap("\x1b[2m", "\x1b[22m", s); }   // dim
    std::string code(const std::string& s) const { return wrap("\x1b[7m", "\x1b[27m", s); }   // inline code (inverse)
    std::string math(const std::string& s) const { return wrap("\x1b[36m", "\x1b[39m", s); }  // math (cyan)
    std::string head(const std::string& s) const { return wrap("\x1b[1;4m", "\x1b[0m", s); }  // heading (bold+underline)
};

inline std::string inline_md(const std::string& s, const Style& st) {
    std::string out; size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '`') {                                         // `inline code`
            size_t e = s.find('`', i + 1);
            if (e != std::string::npos) { out += st.code(s.substr(i + 1, e - i - 1)); i = e + 1; continue; }
        }
        if (c == '$') {                                        // $math$ or $$math$$
            bool disp = (i + 1 < n && s[i + 1] == '$');
            std::string close = disp ? "$$" : "$";
            size_t from = i + close.size(), e = s.find(close, from);
            if (e != std::string::npos) { out += st.math(tex_to_unicode(s.substr(from, e - from))); i = e + close.size(); continue; }
        }
        if (c == '\\' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '[')) {   // \( .. \)  or  \[ .. \]
            std::string close = s[i + 1] == '(' ? "\\)" : "\\]";
            size_t e = s.find(close, i + 2);
            if (e != std::string::npos) { out += st.math(tex_to_unicode(s.substr(i + 2, e - i - 2))); i = e + 2; continue; }
        }
        if ((c == '*' && i + 1 < n && s[i + 1] == '*') || (c == '_' && i + 1 < n && s[i + 1] == '_')) {   // **bold**
            std::string mark(2, c); size_t e = s.find(mark, i + 2);
            if (e != std::string::npos) { out += st.b(inline_md(s.substr(i + 2, e - i - 2), st)); i = e + 2; continue; }
        }
        if (c == '*' || c == '_') {                            // *italic*
            size_t e = s.find(c, i + 1);
            if (e != std::string::npos && e > i + 1) { out += st.it(s.substr(i + 1, e - i - 1)); i = e + 1; continue; }
        }
        if (c == '[') {                                        // [text](url)
            size_t rb = s.find(']', i + 1);
            if (rb != std::string::npos && rb + 1 < n && s[rb + 1] == '(') {
                size_t rp = s.find(')', rb + 2);
                if (rp != std::string::npos) {
                    out += st.ul(s.substr(i + 1, rb - i - 1)) + st.dim(" (" + s.substr(rb + 2, rp - rb - 2) + ")");
                    i = rp + 1; continue;
                }
            }
        }
        out += c; i++;
    }
    return out;
}

inline std::string render(const std::string& text, bool color) {
    Style st{color};
    std::string src = strip_mathml(text);
    std::vector<std::string> lines;
    for (size_t p = 0; p <= src.size();) {
        size_t e = src.find('\n', p);
        if (e == std::string::npos) { lines.push_back(src.substr(p)); break; }
        lines.push_back(src.substr(p, e - p)); p = e + 1;
    }
    std::string out; bool in_code = false, in_dmath = false; std::string dmath, dclose;
    for (auto& line : lines) {
        size_t ind = line.find_first_not_of(" \t");
        std::string ls = ind == std::string::npos ? "" : line.substr(ind);
        if (in_dmath) {                                        // inside a $$ / \[ display-math block
            size_t e = line.find(dclose);
            if (e == std::string::npos) { dmath += line + " "; continue; }
            dmath += line.substr(0, e);
            out += "    " + st.math(tex_to_unicode(dmath)) + "\n"; in_dmath = false; dmath.clear(); continue;
        }
        if (ls.rfind("```", 0) == 0 || ls.rfind("~~~", 0) == 0) { in_code = !in_code; continue; }   // fence line dropped
        if (in_code) { out += st.dim("  " + line) + "\n"; continue; }
        if (ls == "$$" || ls == "\\[") { in_dmath = true; dclose = (ls == "$$") ? "$$" : "\\]"; dmath.clear(); continue; }
        if (ls.rfind("#", 0) == 0) {                           // ATX heading
            size_t h = ls.find_first_not_of('#');
            std::string txt = h == std::string::npos ? "" : ls.substr(h);
            if (!txt.empty() && txt[0] == ' ') txt.erase(0, 1);
            out += st.head(inline_md(txt, st)) + "\n"; continue;
        }
        if (ls == "---" || ls == "***" || ls == "___") { out += st.dim("────────────────────") + "\n"; continue; }
        if (ls.rfind("> ", 0) == 0) { out += st.dim("│ " + inline_md(ls.substr(2), st)) + "\n"; continue; }
        if (ls.rfind("- ", 0) == 0 || ls.rfind("* ", 0) == 0 || ls.rfind("+ ", 0) == 0) {
            std::string indent = line.substr(0, ind == std::string::npos ? 0 : ind);
            out += indent + "• " + inline_md(ls.substr(2), st) + "\n"; continue;
        }
        out += inline_md(line, st) + "\n";
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();     // don't add a trailing newline of our own
    return out;
}

}  // namespace mdterm
