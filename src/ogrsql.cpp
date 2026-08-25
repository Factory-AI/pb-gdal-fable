#include "ogrsql.h"
#include "cpl.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{

// ---------------------------------------------------------------- lexer

enum TokKind
{
    TK_END,
    TK_IDENT,
    TK_STRING,
    TK_INT,
    TK_FLOAT,
    TK_KW,
    TK_OP,   // single/double char operator, text holds it
    TK_BAD
};

struct Tok
{
    TokKind kind = TK_END;
    std::string text;   // identifier (unquoted), keyword (upper), op text
    std::string raw;    // as written (identifiers keep case)
    long long i = 0;
    double d = 0;
    size_t pos = 0;
    bool unterminated = false;
};

JVal jint(long long i)
{
    JVal j;
    j.type = JVal::INT;
    j.i = i;
    return j;
}

JVal jstr(const std::string &s)
{
    JVal j;
    j.type = JVal::STRING;
    j.s = s;
    return j;
}

const char *const kKeywords[] = {
    "SELECT", "DISTINCT", "FROM",  "WHERE",  "AND",    "OR",   "NOT",
    "IN",     "LIKE",     "ILIKE", "BETWEEN", "IS",    "NULL", "CAST",
    "AS",     "ORDER",    "BY",    "ASC",    "DESC",   "LIMIT",
    "OFFSET", "UNION",    "JOIN",  "LEFT",   "ON",     "ESCAPE", "ALL"};

bool isKeyword(const std::string &up)
{
    for (const char *k : kKeywords)
        if (up == k)
            return true;
    return false;
}

struct Lexer
{
    const std::string &in;
    size_t p = 0;
    bool bad = false;

    explicit Lexer(const std::string &s) : in(s) {}

    Tok next()
    {
        while (p < in.size() && isspace((unsigned char)in[p]))
            ++p;
        Tok t;
        t.pos = p;
        if (p >= in.size())
        {
            t.kind = TK_END;
            return t;
        }
        char c = in[p];
        if (c == '\'')
        {
            ++p;
            std::string s;
            while (p < in.size())
            {
                if (in[p] == '\'')
                {
                    if (p + 1 < in.size() && in[p + 1] == '\'')
                    {
                        s += '\'';
                        p += 2;
                        continue;
                    }
                    ++p;
                    t.kind = TK_STRING;
                    t.text = s;
                    return t;
                }
                s += in[p++];
            }
            t.kind = TK_BAD;
            t.unterminated = true;
            return t;
        }
        if (c == '"')
        {
            ++p;
            std::string s;
            while (p < in.size())
            {
                if (in[p] == '"')
                {
                    if (p + 1 < in.size() && in[p + 1] == '"')
                    {
                        s += '"';
                        p += 2;
                        continue;
                    }
                    ++p;
                    t.kind = TK_IDENT;
                    t.text = s;
                    t.raw = s;
                    return t;
                }
                s += in[p++];
            }
            t.kind = TK_BAD;
            t.unterminated = true;
            return t;
        }
        if (isdigit((unsigned char)c) ||
            (c == '.' && p + 1 < in.size() &&
             isdigit((unsigned char)in[p + 1])))
        {
            size_t s = p;
            bool isFloat = false;
            while (p < in.size() && isdigit((unsigned char)in[p]))
                ++p;
            if (p < in.size() && in[p] == '.')
            {
                isFloat = true;
                ++p;
                while (p < in.size() && isdigit((unsigned char)in[p]))
                    ++p;
            }
            if (p < in.size() && (in[p] == 'e' || in[p] == 'E'))
            {
                size_t q = p + 1;
                if (q < in.size() && (in[q] == '+' || in[q] == '-'))
                    ++q;
                if (q < in.size() && isdigit((unsigned char)in[q]))
                {
                    isFloat = true;
                    p = q;
                    while (p < in.size() && isdigit((unsigned char)in[p]))
                        ++p;
                }
            }
            std::string num = in.substr(s, p - s);
            if (isFloat)
            {
                t.kind = TK_FLOAT;
                t.d = atof(num.c_str());
            }
            else
            {
                t.kind = TK_INT;
                t.i = atoll(num.c_str());
                t.d = (double)t.i;
            }
            t.text = num;
            return t;
        }
        if (isalpha((unsigned char)c) || c == '_')
        {
            size_t s = p;
            while (p < in.size() &&
                   (isalnum((unsigned char)in[p]) || in[p] == '_' ||
                    in[p] == '$'))
                ++p;
            std::string id = in.substr(s, p - s);
            std::string up = id;
            for (char &ch : up)
                ch = (char)toupper((unsigned char)ch);
            if (isKeyword(up))
            {
                t.kind = TK_KW;
                t.text = up;
                t.raw = id;
            }
            else
            {
                t.kind = TK_IDENT;
                t.text = id;
                t.raw = id;
            }
            return t;
        }
        static const char *ops2[] = {"<>", "<=", ">=", "!="};
        for (const char *o : ops2)
            if (in.compare(p, 2, o) == 0)
            {
                t.kind = TK_OP;
                t.text = o;
                p += 2;
                return t;
            }
        if (strchr("=<>+-*/%(),.", c))
        {
            t.kind = TK_OP;
            t.text = std::string(1, c);
            ++p;
            return t;
        }
        t.kind = TK_BAD;
        t.text = std::string(1, c);
        ++p;
        return t;
    }
};

std::string tokDisplay(const Tok &t)
{
    switch (t.kind)
    {
        case TK_END:
            return "end of string";
        case TK_IDENT:
            return "identifier";
        case TK_STRING:
            return "string";
        case TK_INT:
            return "integer number";
        case TK_FLOAT:
            return "floating point number";
        case TK_KW:
            return t.text;
        case TK_OP:
            return "'" + t.text + "'";
        default:
            return "invalid token";
    }
}

// ------------------------------------------------------------------ AST

enum StaticType
{
    ST_INT,
    ST_INT64,
    ST_REAL,
    ST_STR,
    ST_BOOL,
    ST_NULL
};

enum SpecialField
{
    SP_NONE = 0,
    SP_FID,
    SP_GEOMETRY,
    SP_GEOM_WKT,
    SP_GEOM_AREA,
    SP_STYLE
};

struct Node
{
    enum Op
    {
        COL,
        LIT,
        AND,
        OR,
        NOT,
        CMP,      // funcName holds the operator text (=, <>, <, ...)
        ARITH,    // funcName holds + - * / %
        NEG,
        IN,
        BETWEEN,
        LIKE,     // funcName LIKE or ILIKE
        ISNULL,
        CAST,
        FUNC,     // CONCAT / SUBSTR
        AGG       // MIN MAX COUNT SUM AVG
    } op = LIT;

    bool negate = false;  // NOT IN / NOT LIKE / IS NOT NULL / NOT BETWEEN

    // literal
    int litKind = 0;  // 0 null, 1 int, 2 real, 3 str
    long long ival = 0;
    double dval = 0;
    std::string sval;

    // column
    std::string colTable, colName;  // as written
    int fieldIdx = -1;
    int special = SP_NONE;

    std::string funcName;  // op text / function name (upper)
    bool aggStar = false;
    bool aggDistinct = false;

    // CAST target: 0 int, 1 int64, 2 real, 3 string, 4 smallint,
    // 5 boolean, 6 date, 7 time, 8 timestamp (resolved at bind)
    int castType = 0;
    int castWidth = 0;
    int castPrecision = 0;
    std::string castName;  // typename as written

    int st = ST_NULL;  // static type after bind
    size_t pos = 0;

    std::vector<std::unique_ptr<Node>> kids;
};

using NodeP = std::unique_ptr<Node>;

struct SelectItem
{
    NodeP expr;
    std::string alias;
    bool star = false;
};

struct OrderKey
{
    std::string name;
    bool desc = false;
    int fieldIdx = -1;
    int special = SP_NONE;
    int st = ST_STR;
};

struct SelectStmt
{
    bool distinct = false;
    std::vector<SelectItem> items;
    // 'datasource'.table form: the prefix names a secondary datasource
    bool hasDb = false;
    std::string dbName;
    std::string tableName;   // as written
    std::string tableAlias;  // as written ("" if none)
    NodeP where;
    std::string whereText;
    std::vector<OrderKey> order;
    long long limit = -1;
    long long offset = -1;
};

// --------------------------------------------------------------- parser

struct ParseErr
{
    std::string msg;      // bison-style core message
    size_t pos = 0;
    bool set = false;
    // lexer-level failure with its own one-line message and no
    // "Occurred around" context block
    bool unterminated = false;
};

struct Parser
{
    Lexer lex;
    Tok tok;
    const std::string &text;
    ParseErr err;

    explicit Parser(const std::string &s) : lex(s), text(s)
    {
        tok = lex.next();
    }

    void advance() { tok = lex.next(); }

    bool fail(const std::string &expecting = "")
    {
        if (err.set)
            return false;
        err.set = true;
        err.pos = tok.pos;
        if (tok.kind == TK_BAD && tok.unterminated)
        {
            err.unterminated = true;
            return false;
        }
        err.msg = "syntax error, unexpected " + tokDisplay(tok);
        if (!expecting.empty())
            err.msg += ", expecting " + expecting;
        return false;
    }

    bool isKw(const char *k) const
    {
        return tok.kind == TK_KW && tok.text == k;
    }
    bool isOp(const char *o) const
    {
        return tok.kind == TK_OP && tok.text == o;
    }
    bool eatKw(const char *k)
    {
        if (!isKw(k))
            return false;
        advance();
        return true;
    }
    bool eatOp(const char *o)
    {
        if (!isOp(o))
            return false;
        advance();
        return true;
    }

    // ---- expressions
    NodeP parseExpr() { return parseOr(); }

    NodeP parseOr()
    {
        NodeP l = parseAnd();
        if (!l)
            return nullptr;
        while (isKw("OR"))
        {
            advance();
            NodeP r = parseAnd();
            if (!r)
                return nullptr;
            NodeP n(new Node);
            n->op = Node::OR;
            n->kids.push_back(std::move(l));
            n->kids.push_back(std::move(r));
            l = std::move(n);
        }
        return l;
    }

    NodeP parseAnd()
    {
        NodeP l = parseNot();
        if (!l)
            return nullptr;
        while (isKw("AND"))
        {
            advance();
            NodeP r = parseNot();
            if (!r)
                return nullptr;
            NodeP n(new Node);
            n->op = Node::AND;
            n->kids.push_back(std::move(l));
            n->kids.push_back(std::move(r));
            l = std::move(n);
        }
        return l;
    }

    NodeP parseNot()
    {
        if (isKw("NOT"))
        {
            size_t pos = tok.pos;
            advance();
            NodeP k = parseNot();
            if (!k)
                return nullptr;
            NodeP n(new Node);
            n->op = Node::NOT;
            n->pos = pos;
            n->kids.push_back(std::move(k));
            return n;
        }
        return parsePredicate();
    }

    NodeP parsePredicate()
    {
        NodeP l = parseAdditive();
        if (!l)
            return nullptr;
        for (;;)
        {
            if (tok.kind == TK_OP &&
                (tok.text == "=" || tok.text == "<>" || tok.text == "!=" ||
                 tok.text == "<" || tok.text == ">" || tok.text == "<=" ||
                 tok.text == ">="))
            {
                std::string opTxt = tok.text == "!=" ? "<>" : tok.text;
                size_t pos = tok.pos;
                advance();
                NodeP r = parseAdditive();
                if (!r)
                    return nullptr;
                NodeP n(new Node);
                n->op = Node::CMP;
                n->funcName = opTxt;
                n->pos = pos;
                n->kids.push_back(std::move(l));
                n->kids.push_back(std::move(r));
                l = std::move(n);
                continue;
            }
            bool neg = false;
            size_t negPos = tok.pos;
            if (isKw("NOT"))
            {
                // lookahead for IN / LIKE / ILIKE / BETWEEN
                size_t savedP = lex.p;
                Tok savedTok = tok;
                advance();
                if (isKw("IN") || isKw("LIKE") || isKw("ILIKE") ||
                    isKw("BETWEEN"))
                {
                    neg = true;
                }
                else
                {
                    lex.p = savedP;
                    tok = savedTok;
                    break;
                }
            }
            if (isKw("IN"))
            {
                size_t pos = neg ? negPos : tok.pos;
                advance();
                if (!eatOp("("))
                    return fail("'('"), nullptr;
                NodeP n(new Node);
                n->op = Node::IN;
                n->negate = neg;
                n->pos = pos;
                n->kids.push_back(std::move(l));
                for (;;)
                {
                    NodeP e = parseAdditive();
                    if (!e)
                        return nullptr;
                    n->kids.push_back(std::move(e));
                    if (eatOp(","))
                        continue;
                    if (eatOp(")"))
                        break;
                    return fail("')'"), nullptr;
                }
                l = std::move(n);
                continue;
            }
            if (isKw("LIKE") || isKw("ILIKE"))
            {
                std::string fn = tok.text;
                size_t pos = neg ? negPos : tok.pos;
                advance();
                NodeP r = parseAdditive();
                if (!r)
                    return nullptr;
                NodeP n(new Node);
                n->op = Node::LIKE;
                n->funcName = fn;
                n->negate = neg;
                n->pos = pos;
                n->kids.push_back(std::move(l));
                n->kids.push_back(std::move(r));
                if (eatKw("ESCAPE"))
                {
                    NodeP e = parseAdditive();
                    if (!e)
                        return nullptr;
                    n->kids.push_back(std::move(e));
                }
                l = std::move(n);
                continue;
            }
            if (isKw("BETWEEN"))
            {
                size_t pos = neg ? negPos : tok.pos;
                advance();
                NodeP a = parseAdditive();
                if (!a)
                    return nullptr;
                if (!eatKw("AND"))
                    return fail(), nullptr;
                NodeP b = parseAdditive();
                if (!b)
                    return nullptr;
                NodeP n(new Node);
                n->op = Node::BETWEEN;
                n->negate = neg;
                n->pos = pos;
                n->kids.push_back(std::move(l));
                n->kids.push_back(std::move(a));
                n->kids.push_back(std::move(b));
                l = std::move(n);
                continue;
            }
            if (isKw("IS"))
            {
                size_t pos = tok.pos;
                advance();
                bool isNot = eatKw("NOT");
                if (!eatKw("NULL"))
                    return fail(isNot ? "NULL" : "NULL or NOT"), nullptr;
                NodeP n(new Node);
                n->op = Node::ISNULL;
                n->negate = isNot;
                n->pos = pos;
                n->kids.push_back(std::move(l));
                l = std::move(n);
                continue;
            }
            break;
        }
        return l;
    }

    NodeP parseAdditive()
    {
        NodeP l = parseMultiplicative();
        if (!l)
            return nullptr;
        while (tok.kind == TK_OP && (tok.text == "+" || tok.text == "-"))
        {
            std::string o = tok.text;
            size_t pos = tok.pos;
            advance();
            NodeP r = parseMultiplicative();
            if (!r)
                return nullptr;
            NodeP n(new Node);
            n->op = Node::ARITH;
            n->funcName = o;
            n->pos = pos;
            n->kids.push_back(std::move(l));
            n->kids.push_back(std::move(r));
            l = std::move(n);
        }
        return l;
    }

    NodeP parseMultiplicative()
    {
        NodeP l = parseUnary();
        if (!l)
            return nullptr;
        while (tok.kind == TK_OP &&
               (tok.text == "*" || tok.text == "/" || tok.text == "%"))
        {
            std::string o = tok.text;
            size_t pos = tok.pos;
            advance();
            NodeP r = parseUnary();
            if (!r)
                return nullptr;
            NodeP n(new Node);
            n->op = Node::ARITH;
            n->funcName = o;
            n->pos = pos;
            n->kids.push_back(std::move(l));
            n->kids.push_back(std::move(r));
            l = std::move(n);
        }
        return l;
    }

    NodeP parseUnary()
    {
        if (isOp("-"))
        {
            size_t pos = tok.pos;
            advance();
            NodeP k = parseUnary();
            if (!k)
                return nullptr;
            if (k->op == Node::LIT && k->litKind == 1)
            {
                k->ival = -k->ival;
                k->dval = -k->dval;
                return k;
            }
            if (k->op == Node::LIT && k->litKind == 2)
            {
                k->dval = -k->dval;
                return k;
            }
            // swq folds unary minus into literals; on a string literal
            // that folding is a silent no-op
            if (k->op == Node::LIT && k->litKind == 3)
                return k;
            NodeP n(new Node);
            n->op = Node::NEG;
            n->pos = pos;
            n->kids.push_back(std::move(k));
            return n;
        }
        return parsePrimary();
    }

    NodeP parsePrimary()
    {
        if (tok.kind == TK_INT)
        {
            NodeP n(new Node);
            n->op = Node::LIT;
            n->litKind = 1;
            n->ival = tok.i;
            n->dval = (double)tok.i;
            n->pos = tok.pos;
            advance();
            return n;
        }
        if (tok.kind == TK_FLOAT)
        {
            NodeP n(new Node);
            n->op = Node::LIT;
            n->litKind = 2;
            n->dval = tok.d;
            n->pos = tok.pos;
            advance();
            return n;
        }
        if (tok.kind == TK_STRING)
        {
            NodeP n(new Node);
            n->op = Node::LIT;
            n->litKind = 3;
            n->sval = tok.text;
            n->pos = tok.pos;
            advance();
            return n;
        }
        if (isKw("NULL"))
        {
            NodeP n(new Node);
            n->op = Node::LIT;
            n->litKind = 0;
            n->pos = tok.pos;
            advance();
            return n;
        }
        if (isOp("("))
        {
            advance();
            NodeP e = parseExpr();
            if (!e)
                return nullptr;
            if (!eatOp(")"))
                return fail(), nullptr;
            return e;
        }
        if (isKw("CAST"))
        {
            size_t pos = tok.pos;
            advance();
            if (!eatOp("("))
                return fail("'('"), nullptr;
            NodeP e = parseExpr();
            if (!e)
                return nullptr;
            if (!eatKw("AS"))
                return fail("AS"), nullptr;
            if (tok.kind != TK_IDENT)
                return fail("identifier or HIDDEN"), nullptr;
            NodeP n(new Node);
            n->op = Node::CAST;
            n->pos = pos;
            n->castName = tok.raw;
            n->kids.push_back(std::move(e));
            advance();
            if (eatOp("("))
            {
                if (tok.kind == TK_INT)
                {
                    n->castWidth = (int)tok.i;
                    advance();
                }
                if (eatOp(","))
                {
                    if (tok.kind == TK_INT)
                    {
                        n->castPrecision = (int)tok.i;
                        advance();
                    }
                }
                if (!eatOp(")"))
                    return fail("')'"), nullptr;
            }
            if (!eatOp(")"))
                return fail("')'"), nullptr;
            return n;
        }
        if (tok.kind == TK_IDENT)
        {
            std::string first = tok.raw;
            size_t pos = tok.pos;
            advance();
            if (isOp("("))
            {
                std::string up = first;
                for (char &c : up)
                    c = (char)toupper((unsigned char)c);
                advance();
                NodeP n(new Node);
                n->pos = pos;
                bool agg = up == "MIN" || up == "MAX" || up == "COUNT" ||
                           up == "SUM" || up == "AVG";
                n->op = agg ? Node::AGG : Node::FUNC;
                n->funcName = up;
                if (agg && isOp("*"))
                {
                    advance();
                    n->aggStar = true;
                    if (!eatOp(")"))
                        return fail("')'"), nullptr;
                    return n;
                }
                if (agg && isKw("DISTINCT"))
                {
                    advance();
                    n->aggDistinct = true;
                }
                for (;;)
                {
                    NodeP e = parseExpr();
                    if (!e)
                        return nullptr;
                    n->kids.push_back(std::move(e));
                    if (eatOp(","))
                        continue;
                    break;
                }
                if (!eatOp(")"))
                    return fail("')'"), nullptr;
                return n;
            }
            NodeP n(new Node);
            n->op = Node::COL;
            n->pos = pos;
            if (isOp("."))
            {
                advance();
                if (tok.kind != TK_IDENT)
                    return fail(), nullptr;
                n->colTable = first;
                n->colName = tok.raw;
                advance();
            }
            else
                n->colName = first;
            return n;
        }
        fail();
        return nullptr;
    }

    // ---- statement
    bool parseSelect(SelectStmt &s)
    {
        if (!isKw("SELECT"))
            return fail("SELECT or '('");
        advance();
        if (eatKw("DISTINCT"))
            s.distinct = true;
        else
            eatKw("ALL");
        for (;;)
        {
            SelectItem it;
            if (isOp("*"))
            {
                it.star = true;
                advance();
            }
            else
            {
                it.expr = parseExpr();
                if (!it.expr)
                    return false;
                if (eatKw("AS"))
                {
                    if (tok.kind != TK_IDENT)
                        return fail();
                    it.alias = tok.raw;
                    advance();
                }
                else if (tok.kind == TK_IDENT)
                {
                    it.alias = tok.raw;
                    advance();
                }
            }
            s.items.push_back(std::move(it));
            if (eatOp(","))
                continue;
            break;
        }
        if (!eatKw("FROM"))
            return fail("FROM");
        if (tok.kind != TK_IDENT && tok.kind != TK_STRING)
            return fail("string or identifier or HIDDEN");
        bool firstIsString = tok.kind == TK_STRING;
        std::string first = firstIsString ? tok.text : tok.raw;
        advance();
        if (isOp("."))
        {
            advance();
            if (tok.kind != TK_IDENT)
                return fail("identifier or HIDDEN");
            s.hasDb = true;
            s.dbName = first;
            s.tableName = tok.raw;
            advance();
        }
        else if (firstIsString)
            // a bare string is only valid as a datasource prefix
            return fail("'.'");
        else
            s.tableName = first;
        if (tok.kind == TK_IDENT)
        {
            s.tableAlias = tok.raw;
            advance();
        }
        if (isKw("WHERE"))
        {
            advance();
            size_t wstart = tok.pos;
            s.where = parseExpr();
            if (!s.where)
                return false;
            size_t wend = tok.pos;
            while (wend > wstart && isspace((unsigned char)text[wend - 1]))
                --wend;
            s.whereText = text.substr(wstart, wend - wstart);
        }
        if (isKw("ORDER"))
        {
            advance();
            if (!eatKw("BY"))
                return fail("BY");
            for (;;)
            {
                if (tok.kind != TK_IDENT)
                    return fail("identifier or HIDDEN");
                OrderKey k;
                k.name = tok.raw;
                advance();
                if (eatKw("DESC"))
                    k.desc = true;
                else
                    eatKw("ASC");
                s.order.push_back(k);
                if (eatOp(","))
                    continue;
                break;
            }
        }
        if (isKw("LIMIT"))
        {
            advance();
            if (tok.kind != TK_INT)
                return fail("integer number");
            s.limit = tok.i;
            advance();
        }
        if (isKw("OFFSET"))
        {
            advance();
            if (tok.kind != TK_INT)
                return fail("integer number");
            s.offset = tok.i;
            advance();
        }
        if (tok.kind != TK_END)
            return fail("end of string");
        return true;
    }
};

// bison-style context block: up to 40 bytes either side of the
// offending token, caret indented by min(pos, 40)
std::string parseErrorMessage(const std::string &input, const ParseErr &e)
{
    size_t start = e.pos > 40 ? e.pos - 40 : 0;
    size_t end = e.pos + 40;
    if (end > input.size())
        end = input.size();
    std::string msg = "SQL Expression Parsing Error: " + e.msg +
                      ". Occurred around :\n" +
                      input.substr(start, end - start) + "\n";
    msg += std::string(e.pos - start, ' ') + "^";
    return msg;
}

// ------------------------------------------------------------- binding

struct BindCtx
{
    const OgrLayer *lyr = nullptr;
    std::string tableName;   // FROM name as written ("" for filters)
    std::string tableAlias;
    std::string errMsg;      // first bind error
    bool fieldNotFoundStyleFilter = false;  // filter-style message text
};

int fieldStaticType(const OgrFieldDefn &f)
{
    switch (f.type)
    {
        case OFTInteger:
            return f.subType == OFSTBoolean ? ST_BOOL : ST_INT;
        case OFTInteger64:
            return ST_INT64;
        case OFTReal:
            return ST_REAL;
        default:
            return ST_STR;
    }
}

bool numericSt(int st)
{
    return st == ST_INT || st == ST_INT64 || st == ST_REAL ||
           st == ST_BOOL;
}

bool bindNode(Node &n, BindCtx &c);

bool bindColumn(Node &n, BindCtx &c)
{
    std::string shown =
        n.colTable.empty() ? n.colName : n.colTable + "." + n.colName;
    // the filter engine quotes each name part separately
    std::string quoted =
        n.colTable.empty()
            ? "\"" + n.colName + "\""
            : "\"" + n.colTable + "\".\"" + n.colName + "\"";
    if (!n.colTable.empty())
    {
        bool tableOk =
            strEqualNoCase(n.colTable, c.tableName) ||
            (!c.tableAlias.empty() && strEqualNoCase(n.colTable, c.tableAlias));
        if (!tableOk)
        {
            c.errMsg = c.fieldNotFoundStyleFilter
                           ? quoted +
                                 " not recognised as an available field."
                           : "Unrecognized field name " + shown + ".";
            return false;
        }
    }
    const OgrLayer &L = *c.lyr;
    for (size_t i = 0; i < L.fields.size(); ++i)
        if (strEqualNoCase(L.fields[i].name, n.colName))
        {
            n.fieldIdx = (int)i;
            n.st = fieldStaticType(L.fields[i]);
            return true;
        }
    if (strEqualNoCase(n.colName, "FID"))
    {
        n.special = SP_FID;
        n.st = ST_INT;
        return true;
    }
    if (strEqualNoCase(n.colName, "OGR_GEOMETRY"))
    {
        n.special = SP_GEOMETRY;
        n.st = ST_STR;
        return true;
    }
    if (strEqualNoCase(n.colName, "OGR_GEOM_WKT"))
    {
        n.special = SP_GEOM_WKT;
        n.st = ST_STR;
        return true;
    }
    if (strEqualNoCase(n.colName, "OGR_GEOM_AREA"))
    {
        n.special = SP_GEOM_AREA;
        n.st = ST_REAL;
        return true;
    }
    if (strEqualNoCase(n.colName, "OGR_STYLE"))
    {
        n.special = SP_STYLE;
        n.st = ST_STR;
        return true;
    }
    c.errMsg = c.fieldNotFoundStyleFilter
                   ? quoted + " not recognised as an available field."
                   : "Unrecognized field name " + shown + ".";
    return false;
}

bool typeMismatch(BindCtx &c, const std::string &opTxt)
{
    c.errMsg = "Type mismatch or improper type of arguments to " + opTxt +
               " operator.";
    return false;
}

// coerce a string literal used against a numeric operand; strict parse
// with the reference's failure warning
bool coerceLiteralToNumber(Node &lit, BindCtx &c, const std::string &opTxt)
{
    const char *s = lit.sval.c_str();
    char *end = nullptr;
    double v = strtod(s, &end);
    while (end && *end && isspace((unsigned char)*end))
        ++end;
    if (!end || *end != '\0' || end == s)
    {
        cplErrorStr(CE_Warning, CPLE_NotSupported,
                    "Conversion failed when converting the string value '" +
                        lit.sval + "' to data type float.");
        return typeMismatch(c, opTxt);
    }
    lit.litKind = 2;
    lit.dval = v;
    lit.st = ST_REAL;
    return true;
}

bool bindComparisonOperands(Node &l, Node &r, BindCtx &c,
                            const std::string &opTxt)
{
    int a = l.st, b = r.st;
    if (a == ST_NULL || b == ST_NULL)
        return true;
    bool aNum = numericSt(a), bNum = numericSt(b);
    if (aNum && bNum)
        return true;
    if (!aNum && !bNum)
        return true;  // both strings
    Node &strSide = aNum ? r : l;
    if (strSide.op == Node::LIT && strSide.litKind == 3)
        return coerceLiteralToNumber(strSide, c, opTxt);
    return typeMismatch(c, opTxt);
}

bool bindNode(Node &n, BindCtx &c)
{
    for (auto &k : n.kids)
        if (!bindNode(*k, c))
            return false;
    switch (n.op)
    {
        case Node::COL:
            return bindColumn(n, c);
        case Node::LIT:
            n.st = n.litKind == 0   ? ST_NULL
                   : n.litKind == 1 ? (n.ival > INT_MAX || n.ival < INT_MIN
                                           ? ST_INT64
                                           : ST_INT)
                   : n.litKind == 2 ? ST_REAL
                                    : ST_STR;
            return true;
        case Node::AND:
        case Node::OR:
        case Node::NOT:
            n.st = ST_BOOL;
            return true;
        case Node::CMP:
            if (!bindComparisonOperands(*n.kids[0], *n.kids[1], c,
                                        n.funcName))
                return false;
            n.st = ST_BOOL;
            return true;
        case Node::ARITH:
        {
            int a = n.kids[0]->st, b = n.kids[1]->st;
            if (a == ST_STR || b == ST_STR)
            {
                // '+' doubles as string concatenation, but only when
                // neither side is numeric; no literal coercion here
                if (n.funcName == "+" &&
                    (a == ST_STR || a == ST_NULL) &&
                    (b == ST_STR || b == ST_NULL))
                {
                    n.st = ST_STR;
                    return true;
                }
                return typeMismatch(c, n.funcName);
            }
            if (a == ST_REAL || b == ST_REAL)
                n.st = ST_REAL;
            else if (a == ST_INT64 || b == ST_INT64)
                n.st = ST_INT64;
            else
                n.st = ST_INT;
            return true;
        }
        case Node::NEG:
            // the parser lowers unary minus to (-1) * expr, so the
            // mismatch names the '*' operator
            if (n.kids[0]->st == ST_STR)
                return typeMismatch(c, "*");
            n.st = n.kids[0]->st == ST_BOOL ? ST_INT : n.kids[0]->st;
            return true;
        case Node::IN:
        {
            Node &v = *n.kids[0];
            for (size_t i = 1; i < n.kids.size(); ++i)
            {
                Node &e = *n.kids[i];
                if (v.st == ST_NULL || e.st == ST_NULL)
                    continue;
                bool vNum = numericSt(v.st), eNum = numericSt(e.st);
                if (vNum == eNum)
                    continue;
                Node &strSide = vNum ? e : v;
                if (strSide.op == Node::LIT && strSide.litKind == 3)
                {
                    if (!coerceLiteralToNumber(strSide, c, "IN"))
                        return false;
                }
                else
                    return typeMismatch(c, "IN");
            }
            n.st = ST_BOOL;
            return true;
        }
        case Node::BETWEEN:
        {
            if (!bindComparisonOperands(*n.kids[0], *n.kids[1], c,
                                        "BETWEEN"))
                return false;
            if (!bindComparisonOperands(*n.kids[0], *n.kids[2], c,
                                        "BETWEEN"))
                return false;
            n.st = ST_BOOL;
            return true;
        }
        case Node::LIKE:
            if (numericSt(n.kids[0]->st) || numericSt(n.kids[1]->st))
                return typeMismatch(c, n.funcName);
            n.st = ST_BOOL;
            return true;
        case Node::ISNULL:
            n.st = ST_BOOL;
            return true;
        case Node::CAST:
        {
            std::string ty = n.castName;
            for (char &ch : ty)
                ch = (char)toupper((unsigned char)ch);
            if (ty == "INTEGER")
                n.castType = 0;
            else if (ty == "BIGINT")
                n.castType = 1;
            else if (ty == "FLOAT" || ty == "NUMERIC")
                n.castType = 2;
            else if (ty == "CHARACTER")
                n.castType = 3;
            else if (ty == "SMALLINT")
                n.castType = 4;
            else if (ty == "BOOLEAN")
                n.castType = 5;
            else if (ty == "DATE")
                n.castType = 6;
            else if (ty == "TIME")
                n.castType = 7;
            else if (ty == "TIMESTAMP")
                n.castType = 8;
            else
            {
                c.errMsg = "Unrecognized typename " + n.castName +
                           " in CAST operator.";
                return false;
            }
            switch (n.castType)
            {
                case 0:
                case 4:
                    n.st = ST_INT;
                    break;
                case 1:
                    n.st = ST_INT64;
                    break;
                case 2:
                    n.st = ST_REAL;
                    break;
                case 5:
                    n.st = ST_BOOL;
                    break;
                default:
                    n.st = ST_STR;
                    break;
            }
            return true;
        }
        case Node::FUNC:
            if (n.funcName == "CONCAT")
            {
                for (auto &k : n.kids)
                    if (numericSt(k->st))
                        return typeMismatch(c, "CONCAT");
                n.st = ST_STR;
                return true;
            }
            if (n.funcName == "SUBSTR")
            {
                if (n.kids.size() != 2 && n.kids.size() != 3)
                {
                    c.errMsg = strPrintf(
                        "Expected 2 or 3 arguments to SUBSTR(), but "
                        "got %d.",
                        (int)n.kids.size());
                    return false;
                }
                bool ok = n.kids[0]->st == ST_STR ||
                          n.kids[0]->st == ST_NULL;
                for (size_t i = 1; ok && i < n.kids.size(); ++i)
                {
                    int st = n.kids[i]->st;
                    if (st != ST_INT && st != ST_INT64 &&
                        st != ST_BOOL && st != ST_NULL)
                        ok = false;
                }
                if (!ok)
                {
                    c.errMsg =
                        "Wrong argument type for SUBSTR(), expected "
                        "SUBSTR(string,int,int) or SUBSTR(string,int).";
                    return false;
                }
                n.st = ST_STR;
                return true;
            }
            c.errMsg = "Undefined function '" + n.funcName + "' used.";
            return false;
        case Node::AGG:
            // aggregates are validated by the SELECT layer; in filter
            // context they never bind
            c.errMsg = "Undefined function '" + n.funcName + "' used.";
            return false;
    }
    return false;
}

// ------------------------------------------------------------ evaluate

struct V
{
    char k = 'n';  // n null, i int64, d double, s string
    long long i = 0;
    double d = 0;
    std::string s;

    static V null() { return V(); }
    static V ofI(long long v)
    {
        V r;
        r.k = 'i';
        r.i = v;
        r.d = (double)v;
        return r;
    }
    static V ofD(double v)
    {
        V r;
        r.k = 'd';
        r.d = v;
        return r;
    }
    static V ofS(std::string v)
    {
        V r;
        r.k = 's';
        r.s = std::move(v);
        return r;
    }
};

// tri-state boolean: -1 null, 0 false, 1 true
int truth(const V &v)
{
    if (v.k == 'n')
        return -1;
    if (v.k == 'i')
        return v.i != 0;
    if (v.k == 'd')
        return v.d != 0;
    return atof(v.s.c_str()) != 0;
}

double geomAreaOf(const OgrGeometry &g);

// coords are packed x,y,z regardless of dimensionality
double ringArea(const std::vector<double> &coords)
{
    size_t n = coords.size() / 3;
    if (n < 3)
        return 0;
    double area = 0;
    for (size_t i = 0; i < n; ++i)
    {
        size_t j = (i + 1) % n;
        double xi = coords[i * 3], yi = coords[i * 3 + 1];
        double xj = coords[j * 3], yj = coords[j * 3 + 1];
        area += xi * yj - xj * yi;
    }
    return fabs(area) / 2.0;
}

// mirrors OGR_G_Area(): curves compute the area of their implied ring,
// collections sum their surface/curve members and ignore the rest;
// only bare points and multipoints draw the non-surface warning
double geomAreaOf(const OgrGeometry &g)
{
    switch (g.type)
    {
        case 2:  // linestring / ring
            return ringArea(g.coords);
        case 3:  // polygon: shell minus holes
        {
            double a = 0;
            for (size_t i = 0; i < g.parts.size(); ++i)
            {
                double r = ringArea(g.parts[i].coords);
                a += i == 0 ? r : -r;
            }
            return a;
        }
        case 5:  // multilinestring
        case 6:  // multipolygon
        case 7:  // geometrycollection
        {
            double a = 0;
            for (const auto &p : g.parts)
                if (p.type != 1 && p.type != 4)
                    a += geomAreaOf(p);
            return a;
        }
        default:
            return 0;
    }
}

bool geomAreaWarns(const OgrGeometry &g)
{
    return g.type == 1 || g.type == 4;
}

void warnNonSurfaceArea()
{
    cplErrorStr(CE_Warning, CPLE_AppDefined,
                "OGR_G_Area() called against non-surface geometry type.");
}

std::string geomTypeUpperName(const OgrGeometry &g)
{
    static const char *names[] = {"UNKNOWN",         "POINT",
                                  "LINESTRING",      "POLYGON",
                                  "MULTIPOINT",      "MULTILINESTRING",
                                  "MULTIPOLYGON",    "GEOMETRYCOLLECTION"};
    if (g.type >= 1 && g.type <= 7)
        return names[g.type];
    return "UNKNOWN";
}

struct EvalCtx
{
    const OgrLayer *lyr = nullptr;
    const OgrFeature *feat = nullptr;
    bool selectContext = false;  // SELECT projections surface AREA/STYLE
    // aggregates see AREA through the field-set check: non-surface
    // geometries read as NULL and never warn
    bool summaryContext = false;
};

V fieldValue(const EvalCtx &c, int idx, int st)
{
    const OgrFeature &f = *c.feat;
    if (idx >= (int)f.values.size() || !f.values[idx].set)
        return V::null();
    const JVal &v = f.values[idx].v;
    if (v.type == JVal::NUL)
        return V::null();
    switch (st)
    {
        case ST_INT:
        case ST_BOOL:
        case ST_INT64:
        {
            long long i = v.type == JVal::BOOL     ? (v.b ? 1 : 0)
                          : v.type == JVal::INT    ? v.i
                          : v.type == JVal::DOUBLE ? (long long)v.d
                                                   : atoll(v.s.c_str());
            return V::ofI(i);
        }
        case ST_REAL:
        {
            double d = v.type == JVal::BOOL     ? (v.b ? 1 : 0)
                       : v.type == JVal::INT    ? (double)v.i
                       : v.type == JVal::DOUBLE ? v.d
                                                : atof(v.s.c_str());
            return V::ofD(d);
        }
        default:
        {
            if (v.type == JVal::STRING)
                return V::ofS(v.s);
            if (v.type == JVal::INT)
                return V::ofS(strPrintf("%lld", v.i));
            if (v.type == JVal::DOUBLE)
                return V::ofS(ogrFormatDouble(v.d, 15));
            if (v.type == JVal::BOOL)
                return V::ofS(v.b ? "1" : "0");
            return V::null();
        }
    }
}

V evalNode(const Node &n, const EvalCtx &c);

int strCaseCmp(const std::string &a, const std::string &b)
{
    return strcasecmp(a.c_str(), b.c_str());
}

// like-match, % and _, optionally case-insensitive, with escape char
bool likeMatch(const char *pat, const char *str, char esc, bool ci)
{
    for (; *pat; ++pat)
    {
        if (esc && *pat == esc && pat[1])
        {
            ++pat;
            char pc = ci ? (char)tolower((unsigned char)*pat) : *pat;
            char sc = ci ? (char)tolower((unsigned char)*str) : *str;
            if (pc != sc)
                return false;
            ++str;
            continue;
        }
        if (*pat == '%')
        {
            while (pat[1] == '%')
                ++pat;
            if (!pat[1])
                return true;
            for (const char *s = str; ; ++s)
            {
                if (likeMatch(pat + 1, s, esc, ci))
                    return true;
                if (!*s)
                    return false;
            }
        }
        if (!*str)
            return false;
        if (*pat == '_')
        {
            ++str;
            continue;
        }
        char pc = ci ? (char)tolower((unsigned char)*pat) : *pat;
        char sc = ci ? (char)tolower((unsigned char)*str) : *str;
        if (pc != sc)
            return false;
        ++str;
    }
    return !*str;
}

int compareVals(const V &a, const V &b, const std::string &op)
{
    // returns tri-state of the comparison; -1 when null involved
    if (a.k == 'n' || b.k == 'n')
        return -1;
    int cmp;
    if (a.k == 's' && b.k == 's')
        cmp = strCaseCmp(a.s, b.s) < 0 ? -1 : (strCaseCmp(a.s, b.s) > 0 ? 1 : 0);
    else
    {
        double x = a.k == 's' ? atof(a.s.c_str()) : a.d;
        double y = b.k == 's' ? atof(b.s.c_str()) : b.d;
        if (a.k == 'i' && b.k == 'i')
            cmp = a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
        else
            cmp = x < y ? -1 : (x > y ? 1 : 0);
    }
    if (op == "=")
        return cmp == 0;
    if (op == "<>")
        return cmp != 0;
    if (op == "<")
        return cmp < 0;
    if (op == ">")
        return cmp > 0;
    if (op == "<=")
        return cmp <= 0;
    return cmp >= 0;  // >=
}

std::string valToStr(const V &v)
{
    if (v.k == 's')
        return v.s;
    if (v.k == 'i')
        return strPrintf("%lld", v.i);
    if (v.k == 'd')
        return ogrFormatDouble(v.d, 15);
    return "";
}

V evalNode(const Node &n, const EvalCtx &c)
{
    switch (n.op)
    {
        case Node::COL:
        {
            if (n.special == SP_NONE)
                return fieldValue(c, n.fieldIdx, n.st);
            const OgrFeature &f = *c.feat;
            switch (n.special)
            {
                case SP_FID:
                    return V::ofI(f.fid);
                case SP_GEOMETRY:
                    if (!f.hasGeom)
                        return c.selectContext && !c.summaryContext
                                   ? V::ofS("")
                                   : V::null();
                    return V::ofS(geomTypeUpperName(f.geom));
                case SP_GEOM_WKT:
                    if (!f.hasGeom)
                        return c.selectContext && !c.summaryContext
                                   ? V::ofS("")
                                   : V::null();
                    return V::ofS(ogrWkt(f.geom));
                case SP_GEOM_AREA:
                {
                    if (c.summaryContext)
                    {
                        if (!f.hasGeom)
                            return V::null();
                        if (geomAreaWarns(f.geom))
                            warnNonSurfaceArea();
                        if (f.geom.type != 3 && f.geom.type != 6 &&
                            f.geom.type != 7)
                            return V::null();
                        return V::ofD(geomAreaOf(f.geom));
                    }
                    if (!f.hasGeom)
                        return c.selectContext ? V::ofD(0) : V::null();
                    if (geomAreaWarns(f.geom))
                    {
                        warnNonSurfaceArea();
                        return V::ofD(0);
                    }
                    return V::ofD(geomAreaOf(f.geom));
                }
                case SP_STYLE:
                    return c.selectContext ? V::ofS("") : V::null();
            }
            return V::null();
        }
        case Node::LIT:
            switch (n.litKind)
            {
                case 1:
                    return V::ofI(n.ival);
                case 2:
                    return V::ofD(n.dval);
                case 3:
                    return V::ofS(n.sval);
                default:
                    return V::null();
            }
        case Node::AND:
        {
            int a = truth(evalNode(*n.kids[0], c));
            int b = truth(evalNode(*n.kids[1], c));
            if (a == 0 || b == 0)
                return V::ofI(0);
            if (a == -1 || b == -1)
                return V::null();
            return V::ofI(1);
        }
        case Node::OR:
        {
            int a = truth(evalNode(*n.kids[0], c));
            int b = truth(evalNode(*n.kids[1], c));
            if (a == 1 || b == 1)
                return V::ofI(1);
            if (a == -1 || b == -1)
                return V::null();
            return V::ofI(0);
        }
        case Node::NOT:
        {
            int a = truth(evalNode(*n.kids[0], c));
            if (a == -1)
                return V::null();
            return V::ofI(a ? 0 : 1);
        }
        case Node::CMP:
        {
            int r = compareVals(evalNode(*n.kids[0], c),
                                evalNode(*n.kids[1], c), n.funcName);
            return r < 0 ? V::null() : V::ofI(r);
        }
        case Node::ARITH:
        {
            V a = evalNode(*n.kids[0], c), b = evalNode(*n.kids[1], c);
            if (a.k == 'n' || b.k == 'n')
                return V::null();
            if (n.st == ST_STR)
                return V::ofS(valToStr(a) + valToStr(b));
            if (n.st == ST_REAL)
            {
                double x = a.d, y = b.d;
                if (n.funcName == "+")
                    return V::ofD(x + y);
                if (n.funcName == "-")
                    return V::ofD(x - y);
                if (n.funcName == "*")
                    return V::ofD(x * y);
                // swq turns division/modulus by zero into INT_MAX
                if (y == 0)
                    return V::ofD(INT_MAX);
                if (n.funcName == "/")
                    return V::ofD(x / y);
                return V::ofD(fmod(x, y));
            }
            long long x = a.k == 'i' ? a.i : (long long)a.d;
            long long y = b.k == 'i' ? b.i : (long long)b.d;
            if (n.funcName == "+")
                return V::ofI(x + y);
            if (n.funcName == "-")
                return V::ofI(x - y);
            if (n.funcName == "*")
                return V::ofI(x * y);
            if (y == 0)
                return V::ofI(INT_MAX);
            if (n.funcName == "/")
                return V::ofI(x / y);
            return V::ofI(x % y);
        }
        case Node::NEG:
        {
            V a = evalNode(*n.kids[0], c);
            if (a.k == 'n')
                return a;
            if (a.k == 'd')
                return V::ofD(-a.d);
            return V::ofI(-a.i);
        }
        case Node::IN:
        {
            V v = evalNode(*n.kids[0], c);
            if (v.k == 'n')
                return V::null();
            bool found = false;
            for (size_t i = 1; i < n.kids.size() && !found; ++i)
            {
                V e = evalNode(*n.kids[i], c);
                if (e.k == 'n')
                    continue;
                if (compareVals(v, e, "=") == 1)
                    found = true;
            }
            bool r = n.negate ? !found : found;
            return V::ofI(r ? 1 : 0);
        }
        case Node::BETWEEN:
        {
            int a = compareVals(evalNode(*n.kids[0], c),
                                evalNode(*n.kids[1], c), ">=");
            int b = compareVals(evalNode(*n.kids[0], c),
                                evalNode(*n.kids[2], c), "<=");
            if (a < 0 || b < 0)
                return V::null();
            bool r = a == 1 && b == 1;
            if (n.negate)
                r = !r;
            return V::ofI(r ? 1 : 0);
        }
        case Node::LIKE:
        {
            V v = evalNode(*n.kids[0], c);
            V p = evalNode(*n.kids[1], c);
            if (v.k == 'n' || p.k == 'n')
                return V::null();
            char esc = 0;
            if (n.kids.size() > 2)
            {
                V e = evalNode(*n.kids[2], c);
                if (e.k == 's' && !e.s.empty())
                    esc = e.s[0];
            }
            bool ci = n.funcName == "ILIKE";
            bool r = likeMatch(p.k == 's' ? p.s.c_str() : "",
                               v.k == 's' ? v.s.c_str() : "", esc, ci);
            if (n.negate)
                r = !r;
            return V::ofI(r ? 1 : 0);
        }
        case Node::ISNULL:
        {
            V v = evalNode(*n.kids[0], c);
            bool isNull = v.k == 'n';
            if (n.negate)
                isNull = !isNull;
            return V::ofI(isNull ? 1 : 0);
        }
        case Node::CAST:
        {
            V v = evalNode(*n.kids[0], c);
            if (v.k == 'n')
                return v;
            switch (n.castType)
            {
                case 0:
                case 1:
                case 4:
                {
                    long long i = v.k == 'i'   ? v.i
                                  : v.k == 'd' ? (long long)v.d
                                               : atoll(v.s.c_str());
                    if (n.castType == 4)
                        i = (short)i;
                    return V::ofI(i);
                }
                case 5:
                {
                    long long i = v.k == 'i'   ? v.i
                                  : v.k == 'd' ? (long long)v.d
                                               : atoll(v.s.c_str());
                    return V::ofI(i != 0 ? 1 : 0);
                }
                case 2:
                {
                    double d = v.k == 'i'   ? (double)v.i
                               : v.k == 'd' ? v.d
                                            : atof(v.s.c_str());
                    return V::ofD(d);
                }
                default:
                {
                    // explicit CHARACTER(n) truncates; the default
                    // 1-wide CHARACTER only declares the width
                    std::string s = valToStr(v);
                    if (n.castType == 3 && n.castWidth > 0 &&
                        (int)s.size() > n.castWidth)
                        s.resize(n.castWidth);
                    return V::ofS(s);
                }
            }
        }
        case Node::FUNC:
        {
            if (n.funcName == "CONCAT")
            {
                std::string s;
                for (auto &k : n.kids)
                {
                    V v = evalNode(*k, c);
                    if (v.k == 'n')
                        return V::null();
                    s += valToStr(v);
                }
                return V::ofS(s);
            }
            // SUBSTR: 1-based start; negative start counts from the end
            V sv = evalNode(*n.kids[0], c);
            if (sv.k == 'n')
                return V::null();
            std::string s = valToStr(sv);
            long long start = 1, len = -1;
            if (n.kids.size() > 1)
            {
                V a = evalNode(*n.kids[1], c);
                if (a.k == 'n')
                    return V::null();
                start = a.k == 'i' ? a.i : (long long)a.d;
            }
            if (n.kids.size() > 2)
            {
                V a = evalNode(*n.kids[2], c);
                if (a.k == 'n')
                    return V::null();
                len = a.k == 'i' ? a.i : (long long)a.d;
            }
            long long slen = (long long)s.size();
            long long begin;
            if (start < 0)
                begin = slen + start;
            else if (start > 0)
                begin = start - 1;
            else
                begin = 0;
            if (begin < 0)
                begin = 0;
            if (begin > slen)
                begin = slen;
            long long count = len < 0 ? slen - begin : len;
            if (count < 0)
                count = 0;
            if (begin + count > slen)
                count = slen - begin;
            return V::ofS(s.substr((size_t)begin, (size_t)count));
        }
        default:
            return V::null();
    }
}

// ----------------------------------------------------- shared helpers

void recomputeExtent(OgrLayer &lyr)
{
    bool has = false;
    double e[4] = {0, 0, 0, 0};
    std::function<void(const OgrGeometry &)> scan =
        [&](const OgrGeometry &g) {
            for (size_t i = 0; i + 2 < g.coords.size(); i += 3)
            {
                double x = g.coords[i], y = g.coords[i + 1];
                if (std::isnan(x) || std::isnan(y))
                    continue;
                if (!has)
                {
                    e[0] = e[2] = x;
                    e[1] = e[3] = y;
                    has = true;
                }
                else
                {
                    e[0] = std::min(e[0], x);
                    e[1] = std::min(e[1], y);
                    e[2] = std::max(e[2], x);
                    e[3] = std::max(e[3], y);
                }
            }
            for (const auto &p : g.parts)
                scan(p);
        };
    for (const OgrFeature &f : lyr.features)
        if (f.hasGeom && !f.geom.empty)
            scan(f.geom);
    lyr.hasExtent = has;
    for (int i = 0; i < 4; ++i)
        lyr.extent[i] = e[i];
}

bool parseFilterExpression(const std::string &where, NodeP &out,
                           std::string &parseErr)
{
    Parser p(where);
    out = p.parseExpr();
    if (out && p.tok.kind != TK_END)
    {
        p.fail("end of string");
        out.reset();
    }
    if (!out)
    {
        parseErr = p.err.unterminated
                       ? "Did not find end-of-string character"
                       : parseErrorMessage(where, p.err);
        return false;
    }
    return true;
}

}  // namespace

// -------------------------------------------------- attribute filter

bool ogrApplyAttributeFilter(OgrLayer &lyr, const std::string &where,
                             bool emitSetFilterFailed)
{
    NodeP expr;
    std::string parseErr;
    if (!parseFilterExpression(where, expr, parseErr))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, parseErr);
        if (emitSetFilterFailed)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "SetAttributeFilter(" + where + ") failed.");
        return false;
    }
    BindCtx c;
    c.lyr = &lyr;
    c.fieldNotFoundStyleFilter = true;
    if (!bindNode(*expr, c))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, c.errMsg);
        if (emitSetFilterFailed)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "SetAttributeFilter(" + where + ") failed.");
        return false;
    }
    std::vector<OgrFeature> kept;
    for (OgrFeature &f : lyr.features)
    {
        EvalCtx e;
        e.lyr = &lyr;
        e.feat = &f;
        if (truth(evalNode(*expr, e)) == 1)
            kept.push_back(std::move(f));
    }
    lyr.features = std::move(kept);
    recomputeExtent(lyr);
    return true;
}

// ------------------------------------------------------------ SELECT

namespace
{

std::string aggFieldName(const Node &agg)
{
    std::string base = agg.aggStar ? "*" : agg.kids[0]->colName;
    return agg.funcName + "_" + base;
}

JVal vToJVal(const V &v)
{
    if (v.k == 'i')
        return jint(v.i);
    if (v.k == 'd')
    {
        JVal j;
        j.type = JVal::DOUBLE;
        j.d = v.d;
        return j;
    }
    if (v.k == 's')
        return jstr(v.s);
    return JVal();
}

}  // namespace

std::unique_ptr<OgrLayer> ogrExecuteSql(const OgrDataset &ds,
                                        const std::string &sql)
{
    Parser p(sql);
    SelectStmt st;
    if (!p.parseSelect(st))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    p.err.unterminated
                        ? "Did not find end-of-string character"
                        : parseErrorMessage(sql, p.err));
        return nullptr;
    }

    std::unique_ptr<OgrDataset> secondary;
    const OgrDataset *srcDs = &ds;
    if (st.hasDb)
    {
        std::string err2;
        cplPushQuietHandler();
        secondary = openVectorDataset(st.dbName, err2, {}, {}, false);
        cplPopHandler();
        if (!secondary)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unable to open secondary datasource `" +
                            st.dbName + "' required by JOIN.");
            return nullptr;
        }
        srcDs = secondary.get();
    }

    const OgrLayer *src = nullptr;
    for (const OgrLayer &L : srcDs->layers)
        if (strEqualNoCase(L.name, st.tableName))
        {
            src = &L;
            break;
        }
    if (!src)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "SELECT from table " + st.tableName +
                        " failed, no such table/featureclass.");
        return nullptr;
    }

    BindCtx bc;
    bc.lyr = src;
    bc.tableName = st.tableName;
    bc.tableAlias = st.tableAlias;

    // expand *: fields in schema order (geometry is carried implicitly,
    // but * names the anonymous geometry column)
    struct OutCol
    {
        const Node *expr = nullptr;
        NodeP owned;
        std::string name;
        OgrFieldDefn defn;
        bool isAgg = false;
    };
    std::vector<OutCol> cols;
    bool sawStar = false;
    bool haveAgg = false, havePlain = false;

    int fieldPos = 0;
    for (SelectItem &it : st.items)
    {
        ++fieldPos;
        if (it.star)
        {
            sawStar = true;
            for (size_t i = 0; i < src->fields.size(); ++i)
            {
                OutCol oc;
                oc.owned.reset(new Node);
                oc.owned->op = Node::COL;
                oc.owned->colName = src->fields[i].name;
                oc.owned->fieldIdx = (int)i;
                oc.owned->st = fieldStaticType(src->fields[i]);
                oc.expr = oc.owned.get();
                oc.name = src->fields[i].name;
                oc.defn = src->fields[i];
                cols.push_back(std::move(oc));
            }
            havePlain = true;
            continue;
        }
        Node &e = *it.expr;
        if (e.op == Node::AGG)
        {
            haveAgg = true;
            if (!e.aggStar)
            {
                if (e.kids.size() != 1 || e.kids[0]->op != Node::COL)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Argument of column Summary Function '" +
                                    e.funcName + "' should be a column.");
                    return nullptr;
                }
                if (!bindNode(*e.kids[0], bc))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined, bc.errMsg);
                    return nullptr;
                }
                int ast = e.kids[0]->st;
                if ((e.funcName == "SUM" || e.funcName == "AVG") &&
                    ast == ST_STR)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Use of field function " + e.funcName +
                                    "() on string field " +
                                    e.kids[0]->colName + " illegal.");
                    return nullptr;
                }
            }
        }
        else
        {
            havePlain = true;
            if (!bindNode(e, bc))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined, bc.errMsg);
                return nullptr;
            }
        }
        OutCol oc;
        oc.expr = &e;
        oc.isAgg = e.op == Node::AGG;
        // naming
        if (!it.alias.empty())
            oc.name = it.alias;
        else if (e.op == Node::COL)
            oc.name = e.colTable.empty() ? e.colName
                                         : e.colTable + "." + e.colName;
        else if (e.op == Node::AGG)
            oc.name = aggFieldName(e);
        else if (e.op == Node::CAST && e.kids[0]->op == Node::COL)
            oc.name = e.kids[0]->colName;
        else if (e.op == Node::FUNC && !e.kids.empty() &&
                 e.kids[0]->op == Node::COL)
            oc.name = e.funcName + "_" + e.kids[0]->colName;
        else
            oc.name = strPrintf("FIELD_%d", fieldPos);
        // typing
        OgrFieldDefn d;
        d.name = oc.name;
        if (e.op == Node::COL && e.special == SP_NONE)
            d = src->fields[e.fieldIdx];
        else if (e.op == Node::COL)
        {
            d.type = e.special == SP_FID        ? OFTInteger
                     : e.special == SP_GEOM_AREA ? OFTReal
                                                 : OFTString;
        }
        else if (e.op == Node::AGG)
        {
            if (e.funcName == "COUNT")
                d.type = OFTInteger;
            else if (e.funcName == "AVG")
                d.type = OFTReal;
            else if (e.aggStar)
                d.type = OFTInteger;
            else
            {
                int ast = e.kids[0]->st;
                d.type = ast == ST_REAL    ? OFTReal
                         : ast == ST_INT64 ? OFTInteger64
                         : ast == ST_STR   ? OFTString
                                           : OFTInteger;
                if (e.kids[0]->fieldIdx >= 0)
                {
                    const OgrFieldDefn &sf =
                        src->fields[e.kids[0]->fieldIdx];
                    if (sf.type == OFTDate || sf.type == OFTTime ||
                        sf.type == OFTDateTime)
                        d.type = sf.type;
                }
            }
        }
        else if (e.op == Node::CAST)
        {
            switch (e.castType)
            {
                case 0:
                    d.type = OFTInteger;
                    break;
                case 1:
                    d.type = OFTInteger64;
                    break;
                case 2:
                    d.type = OFTReal;
                    if (e.castWidth > 0)
                    {
                        d.width = e.castWidth;
                        d.precision = e.castPrecision;
                    }
                    break;
                case 3:
                    d.type = OFTString;
                    d.width = e.castWidth > 0 ? e.castWidth : 1;
                    break;
                case 4:
                    d.type = OFTInteger;
                    d.subType = OFSTInt16;
                    break;
                case 5:
                    d.type = OFTInteger;
                    d.subType = OFSTBoolean;
                    break;
                case 6:
                    d.type = OFTDate;
                    break;
                case 7:
                    d.type = OFTTime;
                    break;
                case 8:
                    d.type = OFTDateTime;
                    break;
            }
        }
        else
        {
            switch (e.st)
            {
                case ST_BOOL:
                    d.type = OFTInteger;
                    d.subType = OFSTBoolean;
                    break;
                case ST_INT:
                    d.type = OFTInteger;
                    break;
                case ST_INT64:
                    d.type = OFTInteger64;
                    break;
                case ST_REAL:
                    d.type = OFTReal;
                    break;
                default:
                    d.type = OFTString;
                    break;
            }
        }
        d.name = oc.name;
        oc.defn = d;
        cols.push_back(std::move(oc));
    }

    if (haveAgg && havePlain)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Field list implies mixture of regular recordset mode, "
                    "summary mode or distinct field list mode.");
        return nullptr;
    }
    if (st.distinct && cols.size() > 1)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "SELECT DISTINCT not supported on multiple columns.");
        return nullptr;
    }
    if (st.distinct && haveAgg)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Field list implies mixture of regular recordset mode, "
                    "summary mode or distinct field list mode.");
        return nullptr;
    }

    // WHERE binds against the source schema; its unknown-field message
    // uses the attribute-filter wording, unlike the select list
    bc.fieldNotFoundStyleFilter = true;
    if (st.where && !bindNode(*st.where, bc))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, bc.errMsg);
        return nullptr;
    }
    bc.fieldNotFoundStyleFilter = false;

    // ORDER BY binds against the source schema
    for (OrderKey &k : st.order)
    {
        bool found = false;
        for (size_t i = 0; i < src->fields.size(); ++i)
            if (strEqualNoCase(src->fields[i].name, k.name))
            {
                k.fieldIdx = (int)i;
                k.st = fieldStaticType(src->fields[i]);
                found = true;
                break;
            }
        if (!found && strEqualNoCase(k.name, "FID"))
        {
            k.special = SP_FID;
            k.st = ST_INT64;
            found = true;
        }
        if (!found)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unrecognized field name " + k.name +
                            " in ORDER BY.");
            return nullptr;
        }
    }

    // row pipeline: filter -> order -> offset/limit -> project
    std::vector<const OgrFeature *> rows;
    for (const OgrFeature &f : src->features)
    {
        if (st.where)
        {
            EvalCtx e;
            e.lyr = src;
            e.feat = &f;
            if (truth(evalNode(*st.where, e)) != 1)
                continue;
        }
        rows.push_back(&f);
    }

    if (!st.order.empty())
    {
        std::stable_sort(
            rows.begin(), rows.end(),
            [&](const OgrFeature *a, const OgrFeature *b) {
                for (const OrderKey &k : st.order)
                {
                    Node col;
                    col.op = Node::COL;
                    col.fieldIdx = k.fieldIdx;
                    col.special = k.special;
                    col.st = k.st;
                    EvalCtx ea, eb;
                    ea.lyr = eb.lyr = src;
                    ea.feat = a;
                    eb.feat = b;
                    V va = evalNode(col, ea), vb = evalNode(col, eb);
                    int cmp;
                    if (va.k == 'n' && vb.k == 'n')
                        cmp = 0;
                    else if (va.k == 'n')
                        cmp = -1;
                    else if (vb.k == 'n')
                        cmp = 1;
                    else if (k.st == ST_STR)
                    {
                        int r = strcmp(va.s.c_str(), vb.s.c_str());
                        cmp = r < 0 ? -1 : (r > 0 ? 1 : 0);
                    }
                    else if (va.k == 'i' && vb.k == 'i')
                        cmp = va.i < vb.i ? -1 : (va.i > vb.i ? 1 : 0);
                    else
                        cmp = va.d < vb.d ? -1 : (va.d > vb.d ? 1 : 0);
                    if (cmp != 0)
                        return k.desc ? cmp > 0 : cmp < 0;
                }
                return false;
            });
    }

    auto lyr = std::make_unique<OgrLayer>();
    lyr->name = st.tableAlias.empty() ? st.tableName : st.tableAlias;
    for (const OutCol &oc : cols)
        lyr->fields.push_back(oc.defn);

    if (haveAgg)
    {
        lyr->geomType = 101;
        lyr->hasGeomField = false;
        lyr->hasSrs = false;
        OgrFeature out;
        out.fid = 0;
        for (const OutCol &oc : cols)
        {
            const Node &agg = *oc.expr;
            OgrFieldValue fv;
            long long count = 0;
            bool haveMin = false;
            V minV, maxV;
            double sum = 0;
            long long isum = 0;
            bool anyNum = false;
            std::vector<std::string> distinctSeen;
            for (const OgrFeature *f : rows)
            {
                if (agg.aggStar)
                {
                    ++count;
                    continue;
                }
                EvalCtx e;
                e.lyr = src;
                e.feat = f;
                e.selectContext = true;
                e.summaryContext = true;
                V v = evalNode(*agg.kids[0], e);
                if (v.k == 'n')
                    continue;
                if (agg.aggDistinct)
                {
                    std::string key = valToStr(v);
                    bool dup = false;
                    for (const auto &s : distinctSeen)
                        if (s == key)
                        {
                            dup = true;
                            break;
                        }
                    if (dup)
                        continue;
                    distinctSeen.push_back(key);
                }
                ++count;
                if (!haveMin)
                {
                    minV = maxV = v;
                    haveMin = true;
                }
                else
                {
                    if (v.k == 's')
                    {
                        if (strcmp(v.s.c_str(), minV.s.c_str()) < 0)
                            minV = v;
                        if (strcmp(v.s.c_str(), maxV.s.c_str()) > 0)
                            maxV = v;
                    }
                    else
                    {
                        if (v.d < minV.d || (v.k == 'i' && minV.k == 'i' &&
                                             v.i < minV.i))
                            minV = v;
                        if (v.d > maxV.d || (v.k == 'i' && maxV.k == 'i' &&
                                             v.i > maxV.i))
                            maxV = v;
                    }
                }
                if (v.k == 'i')
                {
                    isum += v.i;
                    sum += (double)v.i;
                    anyNum = true;
                }
                else if (v.k == 'd')
                {
                    sum += v.d;
                    anyNum = true;
                }
            }
            if (agg.funcName == "COUNT")
            {
                fv.set = true;
                fv.v = jint(count);
            }
            else if (agg.funcName == "MIN" && haveMin)
            {
                fv.set = true;
                fv.v = vToJVal(minV);
            }
            else if (agg.funcName == "MAX" && haveMin)
            {
                fv.set = true;
                fv.v = vToJVal(maxV);
            }
            else if (agg.funcName == "SUM" && anyNum)
            {
                fv.set = true;
                if (oc.defn.type == OFTReal)
                    fv.v = vToJVal(V::ofD(sum));
                else
                    fv.v = jint(isum);
            }
            else if (agg.funcName == "AVG" && count > 0 && anyNum)
            {
                fv.set = true;
                fv.v = vToJVal(V::ofD(sum / (double)count));
            }
            out.values.push_back(std::move(fv));
        }
        lyr->features.push_back(std::move(out));
        return lyr;
    }

    if (st.distinct)
    {
        lyr->geomType = 101;
        lyr->hasGeomField = false;
        lyr->hasSrs = false;
        const Node &e = *cols[0].expr;
        std::vector<std::pair<bool, std::string>> seen;  // (isNull, key)
        long long fid = 0;
        for (const OgrFeature *f : rows)
        {
            EvalCtx ec;
            ec.lyr = src;
            ec.feat = f;
            ec.selectContext = true;
            V v = evalNode(e, ec);
            bool isNull = v.k == 'n';
            std::string key = isNull ? "" : valToStr(v);
            bool dup = false;
            for (const auto &s : seen)
                if (s.first == isNull && s.second == key)
                {
                    dup = true;
                    break;
                }
            if (dup)
                continue;
            seen.emplace_back(isNull, key);
            OgrFeature out;
            out.fid = fid++;
            OgrFieldValue fv;
            fv.set = true;
            fv.v = isNull ? JVal() : vToJVal(v);
            out.values.push_back(std::move(fv));
            lyr->features.push_back(std::move(out));
        }
        return lyr;
    }

    // regular record-set mode
    lyr->geomType = src->geomType;
    lyr->geomHasZ = src->geomHasZ;
    lyr->geomHasM = src->geomHasM;
    lyr->hasGeomField = src->hasGeomField;
    lyr->hasSrs = src->hasSrs;
    if (src->hasSrs)
        lyr->srs = src->srs.clone();
    if (sawStar)
        lyr->geomColumnName = "_ogr_geometry_";

    // extent: WHERE-filtered rows recompute; an unfiltered select, or
    // any select over an exhausted mid-write capture, forwards the
    // source layer's stored extent
    if (!st.where || ds.capturedStream)
    {
        lyr->hasExtent = src->hasExtent;
        for (int i = 0; i < 4; ++i)
            lyr->extent[i] = src->extent[i];
    }
    else
    {
        OgrLayer tmp;
        for (const OgrFeature *f : rows)
            tmp.features.push_back(*f);
        recomputeExtent(tmp);
        lyr->hasExtent = tmp.hasExtent;
        for (int i = 0; i < 4; ++i)
            lyr->extent[i] = tmp.extent[i];
    }

    size_t begin = st.offset > 0 ? (size_t)st.offset : 0;
    if (begin > rows.size())
        begin = rows.size();
    size_t end = rows.size();
    if (st.limit >= 0 && begin + (size_t)st.limit < end)
        end = begin + (size_t)st.limit;

    // the feature style string is only populated when the output column
    // is itself named OGR_STYLE; an alias suppresses it
    bool projectsStyle = false;
    for (const OutCol &oc : cols)
        if (oc.expr->op == Node::COL && oc.expr->special == SP_STYLE &&
            strEqualNoCase(oc.name, "OGR_STYLE"))
            projectsStyle = true;

    for (size_t r = begin; r < end; ++r)
    {
        const OgrFeature &f = *rows[r];
        OgrFeature out;
        out.fid = f.fid;
        out.explicitFid = f.explicitFid;
        out.hasGeom = f.hasGeom;
        out.geom = f.geom;
        if (projectsStyle)
        {
            out.hasStyle = true;
            out.style = "";
        }
        for (const OutCol &oc : cols)
        {
            const Node &e = *oc.expr;
            OgrFieldValue fv;
            if (e.op == Node::COL && e.special == SP_NONE)
            {
                if (e.fieldIdx >= 0 &&
                    e.fieldIdx < (int)f.values.size())
                    fv = f.values[e.fieldIdx];
            }
            else
            {
                EvalCtx ec;
                ec.lyr = src;
                ec.feat = &f;
                ec.selectContext = true;
                V v = evalNode(e, ec);
                if (v.k != 'n')
                {
                    fv.set = true;
                    fv.v = vToJVal(v);
                }
            }
            out.values.push_back(std::move(fv));
        }
        lyr->features.push_back(std::move(out));
    }
    return lyr;
}
