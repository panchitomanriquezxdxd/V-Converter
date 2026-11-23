#include "Haxe.hpp"

#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>

namespace vscript {

namespace {

enum class TT { Eof, Ident, Int, Float, Str, Regex, Op };

struct Tok {
    TT t = TT::Eof;
    std::string s;
    double num = 0;
    bool single = false;
    std::vector<HxStrPart> parts;
    std::vector<std::pair<std::string, size_t>> exprs;
    int line = 1;
    size_t start = 0;
    size_t end = 0;
};

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void appendUtf8(std::string& out, unsigned long code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

class Lexer {
public:
    explicit Lexer(const std::string& src, size_t base = 0, int line = 1) : src_(src), base_(base), line_(line) {
        if (base == 0 && src_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;
    }

    std::vector<Tok> run() {
        std::vector<Tok> out;
        for (;;) {
            Tok t = next();
            out.push_back(t);
            if (t.t == TT::Eof) break;
        }
        return out;
    }

private:
    const std::string& src_;
    size_t base_;
    size_t i_ = 0;
    int line_;

    char peek(size_t o = 0) const { return i_ + o < src_.size() ? src_[i_ + o] : '\0'; }

    void skipSpace() {
        for (;;) {
            char c = peek();
            if (c == '\n') {
                line_++;
                i_++;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                i_++;
            } else if (c == '/' && peek(1) == '/') {
                while (i_ < src_.size() && src_[i_] != '\n') i_++;
            } else if (c == '/' && peek(1) == '*') {
                i_ += 2;
                while (i_ < src_.size() && !(src_[i_] == '*' && peek(1) == '/')) {
                    if (src_[i_] == '\n') line_++;
                    i_++;
                }
                i_ = i_ + 2 > src_.size() ? src_.size() : i_ + 2;
            } else if (c == '#' && identStart(peek(1))) {
                while (i_ < src_.size() && src_[i_] != '\n') i_++;
            } else if (c == '@') {
                i_++;
                if (peek() == ':') i_++;
                while (identPart(peek()) || peek() == '.') i_++;
                if (peek() == '(') {
                    int depth = 0;
                    while (i_ < src_.size()) {
                        char d = src_[i_++];
                        if (d == '\n') line_++;
                        if (d == '(') depth++;
                        else if (d == ')' && --depth == 0) break;
                    }
                }
            } else {
                break;
            }
        }
    }

    Tok make(TT t, size_t start) {
        Tok tok;
        tok.t = t;
        tok.line = line_;
        tok.start = base_ + start;
        return tok;
    }

    void escape(std::string& out, char quote) {
        char e = src_[i_++];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            case '\'': out += '\''; break;
            case '$': out += '$'; break;
            case 'x': {
                std::string hex = src_.substr(i_, 2);
                i_ += hex.size();
                out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
                break;
            }
            case 'u': {
                std::string hex;
                if (peek() == '{') {
                    i_++;
                    while (i_ < src_.size() && src_[i_] != '}') hex += src_[i_++];
                    i_++;
                } else {
                    hex = src_.substr(i_, 4);
                    i_ += hex.size();
                }
                appendUtf8(out, std::strtoul(hex.c_str(), nullptr, 16));
                break;
            }
            default:
                (void)quote;
                out += e;
        }
    }

    Tok next() {
        skipSpace();
        size_t start = i_;
        if (i_ >= src_.size()) {
            Tok t = make(TT::Eof, start);
            t.end = base_ + i_;
            return t;
        }
        char c = src_[i_];
        if (identStart(c)) {
            Tok t = make(TT::Ident, start);
            while (identPart(peek())) i_++;
            t.s = src_.substr(start, i_ - start);
            t.end = base_ + i_;
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
            Tok t = make(TT::Int, start);
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                i_ += 2;
                while (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_') i_++;
                std::string digits;
                for (size_t k = start + 2; k < i_; k++) if (src_[k] != '_') digits += src_[k];
                t.num = static_cast<double>(std::strtoull(digits.c_str(), nullptr, 16));
            } else {
                bool isFloat = false;
                while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_') i_++;
                if (peek() == '.' && peek(1) != '.' && !identStart(peek(1))) {
                    isFloat = true;
                    i_++;
                    while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_') i_++;
                }
                if (peek() == 'e' || peek() == 'E') {
                    isFloat = true;
                    i_++;
                    if (peek() == '+' || peek() == '-') i_++;
                    while (std::isdigit(static_cast<unsigned char>(peek()))) i_++;
                }
                std::string digits;
                for (size_t k = start; k < i_; k++) if (src_[k] != '_') digits += src_[k];
                t.num = std::strtod(digits.c_str(), nullptr);
                if (isFloat) t.t = TT::Float;
            }
            t.s = src_.substr(start, i_ - start);
            t.end = base_ + i_;
            return t;
        }
        if (c == '"' || c == '\'') {
            Tok t = make(TT::Str, start);
            t.single = c == '\'';
            i_++;
            std::string text;
            while (i_ < src_.size() && src_[i_] != c) {
                char d = src_[i_];
                if (d == '\\') {
                    i_++;
                    if (i_ < src_.size()) escape(text, c);
                    continue;
                }
                if (d == '\n') line_++;
                if (t.single && d == '$') {
                    if (peek(1) == '$') {
                        text += '$';
                        i_ += 2;
                        continue;
                    }
                    if (peek(1) == '{') {
                        HxStrPart literal;
                        literal.text = text;
                        t.parts.push_back(literal);
                        text.clear();
                        i_ += 2;
                        size_t exprStart = i_;
                        int depth = 1;
                        while (i_ < src_.size()) {
                            char e = src_[i_];
                            if (e == '"' || e == '\'') {
                                char q = e;
                                i_++;
                                while (i_ < src_.size() && src_[i_] != q) i_ += src_[i_] == '\\' ? 2 : 1;
                                i_++;
                                continue;
                            }
                            if (e == '{') depth++;
                            if (e == '}' && --depth == 0) break;
                            i_++;
                        }
                        HxStrPart part;
                        part.isExpr = true;
                        part.text = src_.substr(exprStart, i_ - exprStart);
                        t.parts.push_back(part);
                        t.exprs.push_back({part.text, base_ + exprStart});
                        i_++;
                        continue;
                    }
                    if (identStart(peek(1))) {
                        HxStrPart literal;
                        literal.text = text;
                        t.parts.push_back(literal);
                        text.clear();
                        i_++;
                        size_t exprStart = i_;
                        while (identPart(peek())) i_++;
                        HxStrPart part;
                        part.isExpr = true;
                        part.text = src_.substr(exprStart, i_ - exprStart);
                        t.parts.push_back(part);
                        t.exprs.push_back({part.text, base_ + exprStart});
                        continue;
                    }
                }
                text += d;
                i_++;
            }
            i_++;
            if (!t.parts.empty()) {
                HxStrPart literal;
                literal.text = text;
                t.parts.push_back(literal);
            }
            t.s = text;
            t.end = base_ + (i_ > src_.size() ? src_.size() : i_);
            return t;
        }
        if (c == '~' && peek(1) == '/') {
            Tok t = make(TT::Regex, start);
            i_ += 2;
            while (i_ < src_.size() && src_[i_] != '/') i_ += src_[i_] == '\\' ? 2 : 1;
            i_++;
            while (identPart(peek())) i_++;
            t.s = src_.substr(start, i_ - start);
            t.end = base_ + i_;
            return t;
        }
        static const char* ops[] = {">>>=", "...", "<<=", ">>=", ">>>", "\?\?=", "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=",
            "/=", "%=", "&=", "|=", "^=", "<<", "=>", "->", "?.", "??", nullptr};
        Tok t = make(TT::Op, start);
        for (const char** op = ops; *op; op++) {
            size_t n = std::char_traits<char>::length(*op);
            if (src_.compare(i_, n, *op) == 0) {
                if (std::string(*op) == "?." && std::isdigit(static_cast<unsigned char>(peek(2)))) continue;
                t.s = *op;
                i_ += n;
                t.end = base_ + i_;
                return t;
            }
        }
        t.s = std::string(1, c);
        i_++;
        t.end = base_ + i_;
        return t;
    }
};

const std::vector<std::vector<std::string>> kLevels = {
    {"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", ">>>=", "|=", "&=", "^=", "\?\?="},
    {"=>"},
    {"??"},
    {"||"},
    {"&&"},
    {"..."},
    {"==", "!=", ">", "<", ">=", "<=", "is"},
    {"|", "&", "^"},
    {"<<", ">>", ">>>"},
    {"+", "-"},
    {"*", "/"},
    {"%"},
};

class Parser {
public:
    Parser(const std::string& src, size_t base = 0, int line = 1) {
        Lexer lexer(src, base, line);
        toks_ = lexer.run();
    }

    HxScript script() {
        HxScript out;
        std::vector<ClassInfo> classes;
        while (!at(TT::Eof)) {
            if (isWord("package")) {
                skipTo(";");
                continue;
            }
            if (isWord("import") || isWord("using")) {
                next();
                std::string path;
                while (!at(TT::Eof) && !isOp(";")) {
                    if (isWord("as") || isWord("in")) {
                        next();
                        path += " as " + next().s;
                        continue;
                    }
                    path += next().s;
                }
                acceptOp(";");
                out.imports.push_back(path);
                continue;
            }
            skipModifiers();
            if (isWord("class")) {
                next();
                ClassInfo info;
                info.name = expectIdent();
                if (isOp("<")) typeText();
                while (!isOp("{") && !at(TT::Eof)) {
                    if (isWord("extends")) {
                        next();
                        info.parent = typeText();
                        continue;
                    }
                    next();
                }
                expectOp("{");
                while (!isOp("}") && !at(TT::Eof)) {
                    HxMember m;
                    if (member(m)) info.members.push_back(std::move(m));
                }
                acceptOp("}");
                classes.push_back(std::move(info));
                continue;
            }
            if (isWord("class") || isWord("typedef") || isWord("enum") || isWord("abstract") || isWord("interface")) {
                while (!isOp("{") && !isOp(";") && !at(TT::Eof)) next();
                if (isOp("{")) skipBalanced();
                acceptOp(";");
                continue;
            }
            if (isWord("function") && peekTok(1).t == TT::Ident) {
                HxMember m;
                member(m);
                out.members.push_back(std::move(m));
                continue;
            }
            if (isWord("var") || isWord("final")) {
                int line = cur().line;
                HxPtr v = statement();
                for (size_t i = 0; i < v->names.size(); i++) {
                    HxMember m;
                    m.name = v->names[i];
                    m.type = i < v->keys.size() ? v->keys[i] : "";
                    m.value = v->list[i];
                    m.line = line;
                    out.members.push_back(std::move(m));
                }
                continue;
            }
            out.topLevel.push_back(statement());
        }
        mergeClasses(out, classes);
        return out;
    }

    HxPtr expressionOnly() {
        HxPtr e = expr();
        return e;
    }

private:
    struct ClassInfo {
        std::string name;
        std::string parent;
        std::vector<HxMember> members;
    };

    static void walkNodes(const HxPtr& n, const std::function<void(const HxPtr&)>& fn) {
        if (!n) return;
        fn(n);
        walkNodes(n->a, fn);
        walkNodes(n->b, fn);
        walkNodes(n->c, fn);
        for (const HxPtr& c : n->list) walkNodes(c, fn);
        for (const HxStrPart& p : n->parts) walkNodes(p.expr, fn);
        for (const HxCase& c : n->cases) {
            for (const HxPtr& v : c.values) walkNodes(v, fn);
            walkNodes(c.guard, fn);
            for (const HxPtr& b : c.body) walkNodes(b, fn);
        }
        for (const HxCatch& c : n->catches) for (const HxPtr& b : c.body) walkNodes(b, fn);
        for (const HxParam& p : n->params) walkNodes(p.value, fn);
    }

    static void walkMember(HxMember& m, const std::function<void(const HxPtr&)>& fn) {
        walkNodes(m.value, fn);
        for (const HxPtr& s : m.body) walkNodes(s, fn);
        for (const HxParam& p : m.params) walkNodes(p.value, fn);
    }

    // A file can hold several classes: the one that extends a V-Slice base (Module, Stage, Song...) is the script, and the
    // static functions and variables of the others (helpers) are merged in as "Helper_name" so calls like Helper.clamp(x)
    // keep working. Before, only the first class was read, so a helper class placed first replaced the real one.
    static void mergeClasses(HxScript& out, std::vector<ClassInfo>& classes) {
        if (classes.empty()) return;
        size_t primary = 0;
        for (size_t i = 0; i < classes.size(); i++) {
            if (!classes[i].parent.empty()) {
                primary = i;
                break;
            }
        }
        out.hasClass = true;
        out.className = classes[primary].name;
        out.parent = classes[primary].parent;

        std::set<std::string> helperNames;
        std::map<std::string, std::set<std::string>> helperMembers;
        for (size_t i = 0; i < classes.size(); i++) {
            if (i == primary) continue;
            helperNames.insert(classes[i].name);
            for (const HxMember& m : classes[i].members) if (m.isStatic && m.name != "new") helperMembers[classes[i].name].insert(m.name);
        }

        std::vector<HxMember> merged;
        for (size_t i = 0; i < classes.size(); i++) {
            if (i == primary) continue;
            const std::string& cls = classes[i].name;
            const std::set<std::string>& names = helperMembers[cls];
            for (HxMember& m : classes[i].members) {
                if (!names.count(m.name)) continue;
                std::set<std::string> local;
                for (const HxParam& p : m.params) local.insert(p.name);
                walkMember(m, [&](const HxPtr& n) {
                    if (n->k == HxK::Var) for (const std::string& v : n->names) local.insert(v);
                });
                walkMember(m, [&](const HxPtr& n) {
                    if (n->k == HxK::Ident && names.count(n->s) && !local.count(n->s)) n->s = cls + "_" + n->s;
                });
                m.name = cls + "_" + m.name;
                merged.push_back(std::move(m));
            }
        }
        for (HxMember& m : classes[primary].members) merged.push_back(std::move(m));

        for (HxMember& m : merged) {
            walkMember(m, [&](const HxPtr& n) {
                if (n->k != HxK::Field || !n->a || n->a->k != HxK::Ident || !helperNames.count(n->a->s)) return;
                if (!helperMembers[n->a->s].count(n->s)) return;
                n->s = n->a->s + "_" + n->s;
                n->k = HxK::Ident;
                n->a = nullptr;
            });
        }
        out.members = std::move(merged);
    }

    std::vector<Tok> toks_;
    size_t p_ = 0;

    const Tok& cur() const { return toks_[p_]; }
    const Tok& peekTok(size_t o) const { return toks_[p_ + o < toks_.size() ? p_ + o : toks_.size() - 1]; }
    bool at(TT t) const { return cur().t == t; }
    bool isOp(const char* s) const { return cur().t == TT::Op && cur().s == s; }
    bool isWord(const char* s) const { return cur().t == TT::Ident && cur().s == s; }
    Tok next() {
        Tok t = toks_[p_];
        if (p_ + 1 < toks_.size()) p_++;
        return t;
    }
    size_t lastEnd() const { return p_ > 0 ? toks_[p_ - 1].end : 0; }
    bool acceptOp(const char* s) {
        if (!isOp(s)) return false;
        next();
        return true;
    }
    void expectOp(const char* s) {
        if (!acceptOp(s)) fail(std::string("expected '") + s + "' but found '" + cur().s + "'");
    }
    std::string expectIdent() {
        if (!at(TT::Ident)) fail("expected a name but found '" + cur().s + "'");
        return next().s;
    }
    [[noreturn]] void fail(const std::string& message) const { throw HaxeError(cur().line, message); }

    void skipTo(const char* op) {
        while (!at(TT::Eof) && !isOp(op)) next();
        acceptOp(op);
    }

    void skipBalanced() {
        int depth = 0;
        do {
            if (isOp("{") || isOp("(") || isOp("[")) depth++;
            else if (isOp("}") || isOp(")") || isOp("]")) depth--;
            next();
        } while (depth > 0 && !at(TT::Eof));
    }

    void skipModifiers() {
        while (isWord("public") || isWord("private") || isWord("static") || isWord("override") || isWord("inline") || isWord("dynamic") ||
               isWord("macro") || isWord("extern") || (isWord("final") && peekTok(1).t == TT::Ident && (peekTok(1).s == "class" || peekTok(1).s == "function"))) {
            next();
        }
    }

    HxPtr node(HxK k, const Tok& at) {
        HxPtr n = std::make_shared<HxNode>();
        n->k = k;
        n->line = at.line;
        n->start = at.start;
        return n;
    }

    HxPtr done(HxPtr n) {
        n->end = lastEnd();
        if (n->end < n->start) n->end = n->start;
        return n;
    }

    std::string typeText() {
        std::string out;
        if (acceptOp("?")) out += "?";
        if (isOp("(")) {
            out += next().s;
            while (!isOp(")") && !at(TT::Eof)) {
                out += typeText();
                if (isOp(",") || isOp(":")) out += next().s;
                else if (!isOp(")")) out += next().s;
            }
            out += next().s;
        } else if (isOp("{")) {
            size_t start = p_;
            skipBalanced();
            for (size_t i = start; i < p_; i++) out += toks_[i].s;
        } else if (at(TT::Ident)) {
            out += next().s;
            while (isOp(".") && peekTok(1).t == TT::Ident) {
                next();
                out += "." + next().s;
            }
            if (isOp("<")) {
                next();
                out += "<";
                int pending = 0;
                for (;;) {
                    out += typeText();
                    if (acceptOp(",")) {
                        out += ",";
                        continue;
                    }
                    break;
                }
                if (isOp(">")) next();
                else if (isOp(">>")) {
                    toks_[p_].s = ">";
                    pending = 1;
                } else if (isOp(">>>")) {
                    toks_[p_].s = ">>";
                    pending = 2;
                } else if (isOp(">=")) {
                    toks_[p_].s = "=";
                } else if (isOp(">>=")) {
                    toks_[p_].s = ">=";
                }
                (void)pending;
                out += ">";
            }
        }
        if (isOp("->")) {
            next();
            out += "->" + typeText();
        }
        return out;
    }

    std::vector<HxParam> paramList() {
        std::vector<HxParam> params;
        expectOp("(");
        while (!isOp(")") && !at(TT::Eof)) {
            HxParam param;
            acceptOp("?");
            param.name = expectIdent();
            if (acceptOp(":")) param.type = typeText();
            if (acceptOp("=")) param.value = expr();
            params.push_back(param);
            if (!acceptOp(",")) break;
        }
        expectOp(")");
        return params;
    }

    bool member(HxMember& m) {
        m.line = cur().line;
        bool any = true;
        while (any) {
            any = false;
            if (isWord("public") || isWord("private") || isWord("inline") || isWord("dynamic") || isWord("macro") || isWord("extern")) {
                next();
                any = true;
            } else if (isWord("static")) {
                next();
                m.isStatic = true;
                any = true;
            } else if (isWord("override")) {
                next();
                m.isOverride = true;
                any = true;
            }
        }
        m.line = cur().line;
        if (isWord("function")) {
            next();
            m.isFunction = true;
            m.name = expectIdent();
            if (isOp("<")) typeText();
            m.params = paramList();
            if (acceptOp(":")) m.type = typeText();
            if (isOp("{")) {
                HxPtr body = statement();
                m.body = body->list;
            } else if (acceptOp(";")) {
            } else {
                HxPtr e = statement();
                if (e->k == HxK::If || e->k == HxK::While || e->k == HxK::DoWhile || e->k == HxK::For || e->k == HxK::Switch || e->k == HxK::Try ||
                    e->k == HxK::Var || e->k == HxK::Return || e->k == HxK::Throw || e->k == HxK::Call || e->k == HxK::Assign || e->k == HxK::Block ||
                    e->k == HxK::Postfix || (e->k == HxK::Unop && (e->s == "++" || e->s == "--"))) {
                    m.body.push_back(e);
                    return true;
                }
                HxPtr ret = std::make_shared<HxNode>();
                ret->k = HxK::Return;
                ret->line = e->line;
                ret->start = e->start;
                ret->end = e->end;
                ret->a = e;
                m.body.push_back(ret);
            }
            return true;
        }
        if (isWord("var") || isWord("final")) {
            next();
            m.name = expectIdent();
            if (isOp("(")) skipBalanced();
            if (acceptOp(":")) m.type = typeText();
            if (acceptOp("=")) m.value = expr();
            acceptOp(";");
            return true;
        }
        next();
        return false;
    }

    std::vector<HxPtr> blockBody() {
        std::vector<HxPtr> out;
        expectOp("{");
        while (!isOp("}") && !at(TT::Eof)) out.push_back(statement());
        expectOp("}");
        return out;
    }

    HxPtr statement() {
        const Tok start = cur();
        if (isOp(";")) {
            next();
            HxPtr n = node(HxK::Block, start);
            return done(n);
        }
        if (isOp("{") && !looksLikeObject()) {
            HxPtr n = node(HxK::Block, start);
            n->list = blockBody();
            return done(n);
        }
        if (at(TT::Ident)) {
            const std::string& w = cur().s;
            if (w == "var" || w == "final") {
                next();
                HxPtr n = node(HxK::Var, start);
                n->s = w;
                do {
                    n->names.push_back(expectIdent());
                    n->keys.push_back(acceptOp(":") ? typeText() : "");
                    n->list.push_back(acceptOp("=") ? expr() : nullptr);
                } while (acceptOp(","));
                acceptOp(";");
                return done(n);
            }
            if (w == "function" && peekTok(1).t == TT::Ident) {
                next();
                HxPtr n = node(HxK::Var, start);
                n->s = "function";
                n->names.push_back(expectIdent());
                n->keys.push_back("");
                HxPtr f = node(HxK::Func, start);
                f->s = n->names[0];
                if (isOp("<")) typeText();
                f->params = paramList();
                if (acceptOp(":")) f->type = typeText();
                f->b = statement();
                n->list.push_back(done(f));
                return done(n);
            }
            if (w == "return") {
                next();
                HxPtr n = node(HxK::Return, start);
                if (!isOp(";") && !isOp("}")) n->a = expr();
                acceptOp(";");
                return done(n);
            }
            if (w == "break" || w == "continue") {
                next();
                HxPtr n = node(w == "break" ? HxK::Break : HxK::Continue, start);
                acceptOp(";");
                return done(n);
            }
            if (w == "throw") {
                next();
                HxPtr n = node(HxK::Throw, start);
                n->a = expr();
                acceptOp(";");
                return done(n);
            }
            if (w == "if" || w == "while" || w == "do" || w == "for" || w == "switch" || w == "try") {
                HxPtr n = control();
                acceptOp(";");
                return n;
            }
        }
        HxPtr e = expr();
        acceptOp(";");
        return e;
    }

    HxPtr control() {
        const Tok start = cur();
        std::string w = next().s;
        if (w == "if") {
            HxPtr n = node(HxK::If, start);
            expectOp("(");
            n->a = expr();
            expectOp(")");
            n->b = statement();
            if (isOp(";") && peekTok(1).t == TT::Ident && peekTok(1).s == "else") next();
            if (isWord("else")) {
                next();
                n->c = statement();
            }
            return done(n);
        }
        if (w == "while") {
            HxPtr n = node(HxK::While, start);
            expectOp("(");
            n->a = expr();
            expectOp(")");
            n->b = statement();
            return done(n);
        }
        if (w == "do") {
            HxPtr n = node(HxK::DoWhile, start);
            n->b = statement();
            if (!isWord("while")) fail("expected 'while' after a do block");
            next();
            expectOp("(");
            n->a = expr();
            expectOp(")");
            return done(n);
        }
        if (w == "for") {
            HxPtr n = node(HxK::For, start);
            expectOp("(");
            n->names.push_back(expectIdent());
            if (acceptOp("=>")) n->names.push_back(expectIdent());
            if (!isWord("in")) fail("expected 'in' in a for loop");
            next();
            n->a = expr();
            expectOp(")");
            n->b = statement();
            return done(n);
        }
        if (w == "switch") {
            HxPtr n = node(HxK::Switch, start);
            n->a = expr();
            expectOp("{");
            while (!isOp("}") && !at(TT::Eof)) {
                HxCase c;
                if (isWord("default")) {
                    next();
                    c.isDefault = true;
                } else {
                    if (!isWord("case")) fail("expected 'case' in a switch");
                    next();
                    do c.values.push_back(expr());
                    while (acceptOp(",") || acceptOp("|"));
                    if (isWord("if")) {
                        next();
                        expectOp("(");
                        c.guard = expr();
                        expectOp(")");
                    }
                }
                expectOp(":");
                while (!isWord("case") && !isWord("default") && !isOp("}") && !at(TT::Eof)) c.body.push_back(statement());
                n->cases.push_back(c);
            }
            expectOp("}");
            return done(n);
        }
        HxPtr n = node(HxK::Try, start);
        n->b = statement();
        while (isWord("catch")) {
            next();
            HxCatch c;
            expectOp("(");
            c.name = expectIdent();
            if (acceptOp(":")) typeText();
            expectOp(")");
            HxPtr body = statement();
            if (body->k == HxK::Block) c.body = body->list;
            else c.body.push_back(body);
            n->catches.push_back(c);
        }
        return done(n);
    }

    bool looksLikeObject() const {
        const Tok& a = peekTok(1);
        const Tok& b = peekTok(2);
        if (a.t == TT::Op && a.s == "}") return false;
        return (a.t == TT::Ident || a.t == TT::Str) && b.t == TT::Op && b.s == ":";
    }

    HxPtr expr() { return binary(0); }

    int levelOf(const Tok& t) const {
        if (t.t != TT::Op && !(t.t == TT::Ident && t.s == "is")) return -1;
        for (size_t i = 0; i < kLevels.size(); i++) {
            for (const std::string& op : kLevels[i]) if (op == t.s) return static_cast<int>(i);
        }
        if (t.t == TT::Op && t.s == "?") return 100;
        return -1;
    }

    HxPtr binary(int min) {
        HxPtr left = unary();
        for (;;) {
            const Tok& t = cur();
            if (t.t == TT::Op && t.s == "?" && min <= 1) {
                Tok q = next();
                HxPtr n = std::make_shared<HxNode>();
                n->k = HxK::Ternary;
                n->line = q.line;
                n->start = left->start;
                n->a = left;
                n->b = binary(0);
                expectOp(":");
                n->c = binary(1);
                left = done(n);
                continue;
            }
            int level = levelOf(t);
            if (level < 0 || level == 100 || level < min) break;
            Tok op = next();
            HxPtr n = std::make_shared<HxNode>();
            n->line = op.line;
            n->start = left->start;
            n->s = op.s;
            if (level == 0) {
                n->k = HxK::Assign;
                n->a = left;
                n->b = binary(0);
            } else if (op.s == "is") {
                n->k = HxK::Is;
                n->a = left;
                n->type = typeText();
            } else {
                n->k = HxK::Binop;
                n->a = left;
                n->b = binary(level + 1);
            }
            left = done(n);
        }
        return left;
    }

    HxPtr unary() {
        const Tok start = cur();
        if (at(TT::Op) && (cur().s == "!" || cur().s == "-" || cur().s == "~" || cur().s == "++" || cur().s == "--")) {
            HxPtr n = node(HxK::Unop, start);
            n->s = next().s;
            n->a = unary();
            return done(n);
        }
        if (isWord("cast")) {
            next();
            HxPtr n = node(HxK::Cast, start);
            if (isOp("(")) {
                next();
                n->a = expr();
                if (acceptOp(",")) n->type = typeText();
                expectOp(")");
                return postfix(done(n));
            }
            n->a = unary();
            return done(n);
        }
        if (isWord("untyped")) {
            next();
            return unary();
        }
        return postfix(primary());
    }

    HxPtr postfix(HxPtr e) {
        for (;;) {
            const Tok& t = cur();
            if (t.t != TT::Op) break;
            if (t.s == "." || t.s == "?.") {
                bool safe = t.s == "?.";
                next();
                HxPtr n = std::make_shared<HxNode>();
                n->k = HxK::Field;
                n->line = e->line;
                n->start = e->start;
                n->a = e;
                n->safe = safe;
                n->s = expectIdent();
                e = done(n);
            } else if (t.s == "(") {
                next();
                HxPtr n = std::make_shared<HxNode>();
                n->k = HxK::Call;
                n->line = e->line;
                n->start = e->start;
                n->a = e;
                while (!isOp(")") && !at(TT::Eof)) {
                    n->list.push_back(expr());
                    if (!acceptOp(",")) break;
                }
                expectOp(")");
                e = done(n);
            } else if (t.s == "[") {
                next();
                HxPtr n = std::make_shared<HxNode>();
                n->k = HxK::Index;
                n->line = e->line;
                n->start = e->start;
                n->a = e;
                n->b = expr();
                expectOp("]");
                e = done(n);
            } else if (t.s == "++" || t.s == "--") {
                Tok op = next();
                HxPtr n = std::make_shared<HxNode>();
                n->k = HxK::Postfix;
                n->line = op.line;
                n->start = e->start;
                n->s = op.s;
                n->a = e;
                e = done(n);
            } else if (t.s == "!" && e->k == HxK::Ident) {
                break;
            } else {
                break;
            }
        }
        return e;
    }

    bool arrowAhead() const {
        if (!isOp("(")) return false;
        int depth = 0;
        for (size_t i = p_; i < toks_.size(); i++) {
            const Tok& t = toks_[i];
            if (t.t == TT::Eof) return false;
            if (t.t == TT::Op && (t.s == "(" || t.s == "[" || t.s == "{")) depth++;
            if (t.t == TT::Op && (t.s == ")" || t.s == "]" || t.s == "}")) {
                if (--depth == 0) {
                    const Tok& after = toks_[i + 1 < toks_.size() ? i + 1 : i];
                    return after.t == TT::Op && after.s == "->";
                }
            }
        }
        return false;
    }

    HxPtr arrow(const Tok& start, std::vector<HxParam> params) {
        HxPtr f = node(HxK::Func, start);
        f->params = std::move(params);
        expectOp("->");
        f->s = "";
        f->type = "arrow";
        if (isOp("{") && !looksLikeObject()) {
            f->b = statement();
        } else {
            HxPtr body = expr();
            HxPtr ret = std::make_shared<HxNode>();
            ret->k = HxK::Return;
            ret->line = body->line;
            ret->start = body->start;
            ret->end = body->end;
            ret->a = body;
            f->b = ret;
        }
        return done(f);
    }

    HxPtr stringNode(const Tok& t) {
        HxPtr n = node(HxK::Str, t);
        next();
        n->s = t.s;
        n->singleQuoted = t.single;
        if (!t.parts.empty()) {
            size_t e = 0;
            for (const HxStrPart& part : t.parts) {
                HxStrPart copy = part;
                if (part.isExpr) {
                    Parser sub(t.exprs[e].first, t.exprs[e].second, t.line);
                    copy.expr = sub.expressionOnly();
                    e++;
                }
                n->parts.push_back(copy);
            }
        }
        return done(n);
    }

    HxPtr primary() {
        const Tok start = cur();
        switch (start.t) {
            case TT::Eof:
                fail("unexpected end of the script");
            case TT::Int: {
                HxPtr n = node(HxK::Int, start);
                n->num = start.num;
                n->s = start.s;
                next();
                return done(n);
            }
            case TT::Float: {
                HxPtr n = node(HxK::Float, start);
                n->num = start.num;
                n->s = start.s;
                next();
                return done(n);
            }
            case TT::Str:
                return stringNode(start);
            case TT::Regex: {
                HxPtr n = node(HxK::Regex, start);
                n->s = start.s;
                next();
                return done(n);
            }
            case TT::Ident: {
                const std::string& w = start.s;
                if (w == "true" || w == "false" || w == "null" || w == "this" || w == "super") {
                    next();
                    HxPtr n = node(w == "true" ? HxK::True : w == "false" ? HxK::False : w == "null" ? HxK::Null : w == "this" ? HxK::This : HxK::Super, start);
                    return done(n);
                }
                if (w == "new") {
                    next();
                    HxPtr n = node(HxK::New, start);
                    std::string type = typeText();
                    size_t lt = type.find('<');
                    n->s = lt == std::string::npos ? type : type.substr(0, lt);
                    expectOp("(");
                    while (!isOp(")") && !at(TT::Eof)) {
                        n->list.push_back(expr());
                        if (!acceptOp(",")) break;
                    }
                    expectOp(")");
                    return done(n);
                }
                if (w == "function") {
                    next();
                    HxPtr f = node(HxK::Func, start);
                    if (at(TT::Ident)) f->s = next().s;
                    f->params = paramList();
                    if (acceptOp(":")) f->type = typeText();
                    f->b = statement();
                    return done(f);
                }
                if (w == "if" || w == "switch" || w == "try" || w == "for" || w == "while" || w == "do") return control();
                if (w == "var" || w == "final" || w == "return" || w == "throw" || w == "break" || w == "continue") return statement();
                if (peekTok(1).t == TT::Op && peekTok(1).s == "->") {
                    HxParam param;
                    param.name = next().s;
                    return arrow(start, {param});
                }
                next();
                HxPtr n = node(HxK::Ident, start);
                n->s = w;
                return done(n);
            }
            case TT::Op:
                break;
        }
        if (isOp("(")) {
            if (arrowAhead()) {
                std::vector<HxParam> params = paramList();
                return arrow(start, params);
            }
            next();
            HxPtr n = node(HxK::Paren, start);
            n->a = expr();
            if (acceptOp(":")) typeText();
            expectOp(")");
            return done(n);
        }
        if (isOp("[")) {
            next();
            if (isWord("for") || isWord("while")) {
                HxPtr n = node(HxK::Comprehension, start);
                n->a = control();
                expectOp("]");
                return done(n);
            }
            HxPtr n = node(HxK::ArrayLit, start);
            bool map = false;
            while (!isOp("]") && !at(TT::Eof)) {
                HxPtr item = binary(2);
                if (acceptOp("=>")) {
                    map = true;
                    n->list.push_back(item);
                    n->list.push_back(expr());
                } else {
                    n->list.push_back(item);
                }
                if (!acceptOp(",")) break;
            }
            expectOp("]");
            if (map) n->k = HxK::MapLit;
            return done(n);
        }
        if (isOp("{")) {
            if (looksLikeObject() || peekTok(1).s == "}") {
                next();
                HxPtr n = node(HxK::ObjLit, start);
                while (!isOp("}") && !at(TT::Eof)) {
                    Tok key = next();
                    n->keys.push_back(key.s);
                    expectOp(":");
                    n->list.push_back(expr());
                    if (!acceptOp(",")) break;
                }
                expectOp("}");
                return done(n);
            }
            return statement();
        }
        fail("unexpected '" + start.s + "'");
    }
};

}

HxScript parseHaxe(const std::string& source) {
    Parser parser(source);
    return parser.script();
}

}
