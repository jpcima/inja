#ifndef PANTOR_INJA_LEXER_HPP
#define PANTOR_INJA_LEXER_HPP

#include <cctype>
#include <locale>

#include "config.hpp"
#include "token.hpp"
#include "utils.hpp"


namespace inja {

class Lexer {
  enum class State {
    Text,
    ExpressionStart,
    ExpressionBody,
    LineStart,
    LineBody,
    StatementStart,
    StatementBody,
    CommentStart,
    CommentBody
  } m_state;

  enum class Trim {
    None,
    Newline,
    Whitespace,
    Never,
  };

  const LexerConfig& m_config;
  nonstd::string_view m_in;
  size_t m_tok_start;
  size_t m_pos;

 public:
  explicit Lexer(const LexerConfig& config) : m_config(config) {}

  void start(nonstd::string_view in) {
    m_in = in;
    m_tok_start = 0;
    m_pos = 0;
    m_state = State::Text;
  }

  Token scan() {
    m_tok_start = m_pos;

  again:
    if (m_tok_start >= m_in.size()) return make_token(Token::Kind::Eof);

    switch (m_state) {
      default:
      case State::Text: {
        // fast-scan to first open character
        size_t open_start = m_in.substr(m_pos).find_first_of(m_config.open_chars);
        if (open_start == nonstd::string_view::npos) {
          // didn't find open, return remaining text as text token
          m_pos = m_in.size();
          return make_token(Token::Kind::Text);
        }
        m_pos += open_start;

        // try to match one of the opening sequences, and get the close
        nonstd::string_view open_str = m_in.substr(m_pos);
        bool must_lstrip = false;
        bool must_skip_ws = false;
        if (inja::string_view::starts_with(open_str, m_config.expression_open)) {
          m_state = State::ExpressionStart;
          must_skip_ws = char_get(open_str, m_config.expression_open.size()) == '-';
        } else if (inja::string_view::starts_with(open_str, m_config.statement_open)) {
          m_state = State::StatementStart;
          char ch_next = char_get(open_str, m_config.statement_open.size());
          must_lstrip = m_config.lstrip_blocks && ch_next != '+';
          must_skip_ws = ch_next == '-';
        } else if (inja::string_view::starts_with(open_str, m_config.comment_open)) {
          m_state = State::CommentStart;
          char ch_next = char_get(open_str, m_config.comment_open.size());
          must_lstrip = m_config.lstrip_blocks && ch_next != '+';
          must_skip_ws = ch_next == '-';
        } else if ((m_pos == 0 || m_in[m_pos - 1] == '\n') &&
                   inja::string_view::starts_with(open_str, m_config.line_statement)) {
          m_state = State::LineStart;
        } else {
          m_pos += 1; // wasn't actually an opening sequence
          goto again;
        }

        nonstd::string_view text = string_view::slice(m_in, m_tok_start, m_pos);
        if (must_skip_ws)
          text = clear_trailing_whitespace(text);
        else if (must_lstrip)
          text = clear_final_line_if_whitespace(text);

        if (text.empty()) goto again;  // don't generate empty token
        return Token(Token::Kind::Text, text);
      }
      case State::ExpressionStart: {
        m_state = State::ExpressionBody;
        m_pos += m_config.expression_open.size();
        if (char_get(m_in, m_pos) == '-')
          m_pos += 1;
        return make_token(Token::Kind::ExpressionOpen);
      }
      case State::LineStart: {
        m_state = State::LineBody;
        m_pos += m_config.line_statement.size();
        return make_token(Token::Kind::LineStatementOpen);
      }
      case State::StatementStart: {
        m_state = State::StatementBody;
        m_pos += m_config.statement_open.size();
        char ch_next = char_get(m_in, m_pos);
        if (ch_next == '-' || ch_next == '+')
          m_pos += 1;
        return make_token(Token::Kind::StatementOpen);
      }
      case State::CommentStart: {
        m_state = State::CommentBody;
        m_pos += m_config.comment_open.size();
        char ch_next = char_get(m_in, m_pos);
        if (ch_next == '-' || ch_next == '+')
          m_pos += 1;
        return make_token(Token::Kind::CommentOpen);
      }
      case State::ExpressionBody:
        return scan_body(m_config.expression_close, Token::Kind::ExpressionClose);
      case State::LineBody:
        return scan_body("\n", Token::Kind::LineStatementClose, Trim::Never);
      case State::StatementBody:
        return scan_body(m_config.statement_close, Token::Kind::StatementClose,
                         m_config.trim_blocks ? Trim::Newline : Trim::None);
      case State::CommentBody: {
        // fast-scan to comment close
        size_t end = m_in.substr(m_pos).find(m_config.comment_close);
        if (end == nonstd::string_view::npos) {
          m_pos = m_in.size();
          return make_token(Token::Kind::Eof);
        }
        // return the entire comment in the close token
        m_state = State::Text;
        m_pos += end + m_config.comment_close.size();
        Token tok = make_token(Token::Kind::CommentClose);
        if (m_config.trim_blocks)
          skip_newline();
        return tok;
      }
    }
  }

  const LexerConfig& get_config() const { return m_config; }

 private:
  Token scan_body(nonstd::string_view close, Token::Kind closeKind, Trim trim = Trim::None) {
  again:
    // skip whitespace (except for \n as it might be a close)
    if (m_tok_start >= m_in.size()) return make_token(Token::Kind::Eof);
    char ch = m_in[m_tok_start];
    if (ch == ' ' || ch == '\t' || ch == '\r') {
      m_tok_start += 1;
      goto again;
    }

    // check for close
    if (inja::string_view::starts_with(m_in.substr(m_tok_start), close)) {
      m_state = State::Text;
      m_pos = m_tok_start + close.size();
      Token tok = make_token(closeKind);
      if (trim == Trim::Whitespace)
        skip_whitespace();
      else if (trim == Trim::Newline)
        skip_newline();
      return tok;
    }

    // skip \n
    if (ch == '\n') {
      m_tok_start += 1;
      goto again;
    }

    m_pos = m_tok_start + 1;
    if (std::isalpha(ch)) return scan_id();
    switch (ch) {
      case ',':
        return make_token(Token::Kind::Comma);
      case ':':
        return make_token(Token::Kind::Colon);
      case '(':
        return make_token(Token::Kind::LeftParen);
      case ')':
        return make_token(Token::Kind::RightParen);
      case '[':
        return make_token(Token::Kind::LeftBracket);
      case ']':
        return make_token(Token::Kind::RightBracket);
      case '{':
        return make_token(Token::Kind::LeftBrace);
      case '}':
        return make_token(Token::Kind::RightBrace);
      case '>':
        if (m_pos < m_in.size() && m_in[m_pos] == '=') {
          m_pos += 1;
          return make_token(Token::Kind::GreaterEqual);
        }
        return make_token(Token::Kind::GreaterThan);
      case '<':
        if (m_pos < m_in.size() && m_in[m_pos] == '=') {
          m_pos += 1;
          return make_token(Token::Kind::LessEqual);
        }
        return make_token(Token::Kind::LessThan);
      case '=':
        if (m_pos < m_in.size() && m_in[m_pos] == '=') {
          m_pos += 1;
          return make_token(Token::Kind::Equal);
        }
        return make_token(Token::Kind::Unknown);
      case '!':
        if (m_pos < m_in.size() && m_in[m_pos] == '=') {
          m_pos += 1;
          return make_token(Token::Kind::NotEqual);
        }
        return make_token(Token::Kind::Unknown);
      case '\"':
        return scan_string();
      case '-':
        if (trim != Trim::Never &&
            inja::string_view::starts_with(m_in.substr(m_pos), close)) {
          trim = Trim::Whitespace;
          m_tok_start += 1;
          goto again;
        }
        /* fall through */
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        return scan_number();
      case '_':
        return scan_id();
      default:
        return make_token(Token::Kind::Unknown);
    }
  }

  Token scan_id() {
    for (;;) {
      if (m_pos >= m_in.size()) {
        break;
      }
      char ch = m_in[m_pos];
      if (!std::isalnum(ch) && ch != '.' && ch != '/' && ch != '_' && ch != '-') {
        break;
      }
      m_pos += 1;
    }
    return make_token(Token::Kind::Id);
  }

  Token scan_number() {
    for (;;) {
      if (m_pos >= m_in.size()) {
        break;
      }
      char ch = m_in[m_pos];
      // be very permissive in lexer (we'll catch errors when conversion happens)
      if (!std::isdigit(ch) && ch != '.' && ch != 'e' && ch != 'E' && ch != '+' && ch != '-') {
        break;
      }
      m_pos += 1;
    }
    return make_token(Token::Kind::Number);
  }

  Token scan_string() {
    bool escape {false};
    for (;;) {
      if (m_pos >= m_in.size()) break;
      char ch = m_in[m_pos++];
      if (ch == '\\') {
        escape = true;
      } else if (!escape && ch == m_in[m_tok_start]) {
        break;
      } else {
        escape = false;
      }
    }
    return make_token(Token::Kind::String);
  }

  Token make_token(Token::Kind kind) const {
    return Token(kind, string_view::slice(m_in, m_tok_start, m_pos));
  }

  void skip_whitespace() {
    for (bool ws = true; ws && m_pos < m_in.size();) {
      char ch = m_in[m_pos];
      ws = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
      if (ws)
        m_pos += 1;
    }
  }

  void skip_newline() {
    if (m_pos < m_in.size()) {
      char ch = m_in[m_pos];
      if (ch == '\n')
        m_pos += 1;
      else if (ch == '\r') {
        m_pos += 1;
        if (m_pos < m_in.size() && m_in[m_pos] == '\n')
          m_pos += 1;
      }
    }
  }

  static char char_get(nonstd::string_view text, size_t index, char def = '\0') {
    return (index < text.size()) ? text[index] : def;
  };

  static nonstd::string_view clear_trailing_whitespace(nonstd::string_view text)
  {
    for (bool ws = true; ws && !text.empty();) {
      char ch = text.back();
      ws = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
      if (ws)
        text.remove_suffix(1);
    }
    return text;
  }

  static nonstd::string_view clear_final_line_if_whitespace(nonstd::string_view text)
  {
    nonstd::string_view result = text;
    while (!result.empty()) {
      char ch = result.back();
      if (ch == ' ' || ch == '\t')
       result.remove_suffix(1);
      else if (ch == '\n' || ch == '\r')
        break;
      else
        return text;
    }
    return result;
  }
};

}

#endif // PANTOR_INJA_LEXER_HPP
