// jslex.js -- THE SECURITY RULE's view of the page's code (S-WEB, RV4-10, 2026-10-23): its COMMENTS taken out by a lexer
// pass, not by two regexes. The review proved the regexes blind: `el.x.title = "A // next track"; el.x.innerHTML = t;`
// and `const g = "/*"; el.x.innerHTML = t; /* end */` hid the sink from the rule -- a " // " or a "/*" inside a string,
// a template or a regular expression is not a comment. Here strings, templates (their ${ } expressions lexed as code)
// and regex literals are walked as such and KEPT as written: a sink named in a string (el["innerHTML"]) is still seen.
// Comments become spaces; newlines stay (line numbers hold). A regex is told from a division by what precedes the
// slash (an operator, an opening bracket, a keyword like `return`, or nothing); `}` counts as a regex start -- in
// doubt the lexer keeps text, so a mistake can only make the rule STRICTER (fail closed), never hide code.
"use strict";

const BEFORE_REGEX = /(?:^|[^\w$])(?:return|typeof|instanceof|in|of|new|delete|void|throw|case|do|else|yield|await)$/;

function stripComments(src) {
  src = String(src);
  const n = src.length;
  function lex(start, untilBrace) {
    let out = "", i = start, last = "", depth = 0;
    const regexCanStart = () => {
      if (!last) return true;
      if (/[(,=:[!&|?{};+\-*%<>~^}]/.test(last)) return true;
      if (/[\w$]/.test(last)) return BEFORE_REGEX.test(out.replace(/\s+$/, ""));
      return false;                            // ) ] " ' ` . -- a division
    };
    while (i < n) {
      const c = src[i], d = src[i + 1];
      if (untilBrace) {                        // inside a template's ${ }: ends at ITS closing brace
        if (c === "{") depth++;
        else if (c === "}") { if (depth === 0) return { out: out + "}", i: i + 1 }; depth--; }
      }
      if (c === "/" && d === "/") {            // a line comment: to the end of the line (the newline stays)
        while (i < n && src[i] !== "\n") i++;
        continue;
      }
      if (c === "/" && d === "*") {            // a block comment: spaces, its newlines kept
        const e = src.indexOf("*/", i + 2), end = e < 0 ? n : e + 2;
        out += src.slice(i, end).replace(/[^\n]/g, " ");
        i = end;
        continue;
      }
      if (c === "\"" || c === "'") {           // a string: to its closing quote (an escape skips a character)
        let j = i + 1;
        while (j < n && src[j] !== c && src[j] !== "\n") j += src[j] === "\\" ? 2 : 1;
        out += src.slice(i, Math.min(j + 1, n));
        i = j + 1;
        last = c;
        continue;
      }
      if (c === "`") {                         // a template: its text kept, its ${ } expressions lexed as code
        out += c;
        i++;
        while (i < n) {
          const ch = src[i];
          if (ch === "\\") { out += src.slice(i, i + 2); i += 2; continue; }
          if (ch === "`") { out += ch; i++; break; }
          if (ch === "$" && src[i + 1] === "{") { out += "${"; const r = lex(i + 2, true); out += r.out; i = r.i; continue; }
          out += ch;
          i++;
        }
        last = "`";
        continue;
      }
      if (c === "/" && regexCanStart()) {      // a regex literal: to its closing slash (a class [...] may hold one)
        let j = i + 1, cls = false;
        while (j < n && src[j] !== "\n") {
          const ch = src[j];
          if (ch === "\\") { j += 2; continue; }
          if (ch === "[") cls = true;
          else if (ch === "]") cls = false;
          else if (ch === "/" && !cls) break;
          j++;
        }
        out += src.slice(i, Math.min(j + 1, n));
        i = j + 1;
        last = "/";                            // (its flags follow as letters)
        continue;
      }
      out += c;
      if (!/\s/.test(c)) last = c;
      i++;
    }
    return { out, i };
  }
  return lex(0, false).out;
}

module.exports = { stripComments };
