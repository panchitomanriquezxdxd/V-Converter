#include "HaxeLua.hpp"

#include "Haxe.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace vscript {

namespace {

constexpr int P_OR = 1;
constexpr int P_AND = 2;
constexpr int P_CMP = 3;
constexpr int P_CAT = 4;
constexpr int P_ADD = 5;
constexpr int P_MUL = 6;
constexpr int P_UN = 7;
constexpr int P_POW = 8;
constexpr int P_ATOM = 10;

struct Unsupported {
    std::string why;
};

const std::set<std::string> kLuaKeywords = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local",
    "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};

const std::set<std::string> kPsychApi = {"makeLuaSprite", "makeAnimatedLuaSprite", "makeLuaText", "addLuaSprite", "addLuaText", "removeLuaSprite",
    "removeLuaText", "setProperty", "getProperty", "setPropertyFromClass", "getPropertyFromClass", "callMethod", "callMethodFromClass", "playAnim",
    "addAnimationByPrefix", "addAnimationByIndices", "addAnimation", "addOffset", "scaleObject", "setScrollFactor", "setGraphicSize", "updateHitbox",
    "screenCenter", "loadGraphic", "loadFrames", "makeGraphic", "setObjectCamera", "startTween", "doTweenColor", "cancelTween", "runTimer", "cancelTimer",
    "playSound", "playMusic", "stopSound", "pauseSound", "resumeSound", "setSoundVolume", "soundFadeIn", "soundFadeOut", "cameraFlash", "cameraShake",
    "cameraFade", "characterDance", "getRandomInt", "getRandomFloat", "getRandomBool", "debugPrint", "runHaxeCode", "keyboardJustPressed",
    "keyboardPressed", "keyboardReleased", "mouseClicked", "mousePressed", "mouseReleased", "getMouseX", "getMouseY", "getSongPosition",
    "luaSpriteExists", "getColorFromHex", "setTextSize", "setTextString", "setTextFont", "setTextColor", "setTextAlignment", "setTextBorder",
    "getPropertyFromGroup", "setPropertyFromGroup", "close", "startVideo", "endSong", "makeFlxAnimateSprite", "precacheImage", "precacheSound",
    "triggerEvent", "cameraSetTarget", "setHealth", "getHealth", "addHealth"};

const std::set<std::string> kVSliceCallbacks = {"onCreate", "onUpdate", "onBeatHit", "onStepHit", "onCountdownStart", "onCountdownStep",
    "onCountdownEnd", "onSongStart", "onSongEnd", "onNoteHit", "onNoteMiss", "onNoteGhostMiss", "onNoteIncoming", "onSongEvent", "onPause",
    "onResume", "onGameOver", "onDestroy", "handleEvent", "getTitle", "getEventSchema", "new", "onScriptEvent", "onSongLoaded", "onSongRetry",
    "onNoteHoldDrop", "onStateChangeBegin", "onStateChangeEnd", "onSubStateOpenBegin", "onSubStateOpenEnd", "onSubStateCloseBegin",
    "onSubStateCloseEnd", "onFocusLost", "onFocusGained", "onAdd", "onDialogueStart", "onDialogueLine", "onDialogueEnd", "onDialogueSkip",
    "onDialogueCompleteLine", "buildStage", "fetchAssetPaths", "onAnimationFinished", "onAnimationFrame", "onNoteHoldRelease"};

std::string luaStr(const std::string& text) {
    std::string out = "'";
    for (unsigned char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\%03d", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "'";
}

std::string numText(double value) {
    if (std::isinf(value)) return value > 0 ? "math.huge" : "-math.huge";
    if (std::floor(value) == value && std::fabs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.0f", value);
        return buffer;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.15g", value);
    return buffer;
}

std::string lastSegment(const std::string& path) {
    size_t dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}

bool isIdent(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

enum class VK { Lua, Obj, Game, Cls, Ns, Event, Stage };

struct Seg {
    bool lit = true;
    std::string text;
};

struct Val {
    VK k = VK::Lua;
    std::string lua;
    int prec = P_ATOM;
    std::vector<Seg> segs;
    std::string cls;
    std::string path;
    bool str = false;
    bool arr = false;
    bool map = false;
    bool spr = false;
    bool sprArr = false;
    bool text = false;
    bool isNum = false;
    bool call = false;
    bool none = false;
    std::string hex;
    std::string tagKind;
};

Val lua(const std::string& code, int prec = P_ATOM) {
    Val v;
    v.lua = code;
    v.prec = prec;
    return v;
}

Val callVal(const std::string& code) {
    Val v = lua(code);
    v.call = true;
    return v;
}

Val strVal(const std::string& code, int prec = P_ATOM) {
    Val v = lua(code, prec);
    v.str = true;
    return v;
}

Val numVal(const std::string& code, int prec = P_ATOM) {
    Val v = lua(code, prec);
    v.isNum = true;
    return v;
}

Val noneVal() {
    Val v;
    v.none = true;
    return v;
}

Val objLit(const std::string& tag) {
    Val v;
    v.k = VK::Obj;
    v.segs.push_back({true, tag});
    return v;
}

Val objExpr(const std::string& expr) {
    Val v;
    v.k = VK::Obj;
    v.segs.push_back({false, expr});
    return v;
}

Val ns(const std::string& name) {
    Val v;
    v.k = VK::Ns;
    v.cls = name;
    return v;
}

Val gameRoot() {
    Val v;
    v.k = VK::Game;
    return v;
}

Val gameProp(const std::string& name) {
    Val v;
    v.k = VK::Game;
    v.segs.push_back({true, name});
    return v;
}

Val clsVal(const std::string& cls) {
    Val v;
    v.k = VK::Cls;
    v.cls = cls;
    return v;
}

struct Sym {
    std::string lua;
    bool spr = false;
    bool lit = false;
    bool str = false;
    bool arr = false;
    bool map = false;
    bool sprArr = false;
    bool text = false;
    std::string kind;
    std::string extra;
};

struct Out {
    std::vector<std::string> lines;
    int depth = 0;

    void line(const std::string& text) {
        size_t start = 0;
        for (;;) {
            size_t nl = text.find('\n', start);
            std::string part = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (part.empty()) lines.emplace_back();
            else lines.push_back(std::string(static_cast<size_t>(depth) * 2, ' ') + part);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    void open(const std::string& text) {
        line(text);
        depth++;
    }
    void close(const std::string& text = "end") {
        depth--;
        line(text);
    }
    void append(const Out& other) {
        for (const std::string& l : other.lines) lines.push_back(l);
    }
};

struct Callback {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> head;
    std::vector<std::vector<std::string>> bodies;
};

struct CbSpec {
    std::string psych;
    std::vector<std::string> params;
    std::string event;
    bool cancel = false;
    int mustPress = -1;
    bool prepend = false;
};

class Translator {
public:
    Translator(const std::string& source, const HaxeLuaOptions& options) : src_(source), opt_(options) {}

    HaxeLuaResult run() {
        HaxeLuaResult result;
        try {
            script_ = parseHaxe(src_);
        } catch (const HaxeError& e) {
            result.issues.push_back({e.line, std::string("Haxe syntax error: ") + e.what()});
            return result;
        }
        codename_ = opt_.engine == "codename" || !script_.hasClass;
        className_ = script_.hasClass ? script_.className : baseName(opt_.fileName);
        uniq_ = sanitize(className_.empty() ? "Script" : className_);
        kind_ = detectKind();
        for (const std::string& imp : script_.imports) {
            std::string path = imp;
            std::string alias;
            size_t as = imp.find(" as ");
            if (as != std::string::npos) {
                path = imp.substr(0, as);
                alias = imp.substr(as + 4);
            }
            if (path.empty() || path.back() == '*') continue;
            imports_[alias.empty() ? lastSegment(path) : alias] = path;
        }
        targetId_ = detectTarget();
        if (targetId_.empty()) targetId_ = opt_.id.empty() ? className_ : opt_.id;
        readEventInfo();
        prepass();
        build();
        result.ok = true;
        result.source = assemble();
        result.kind = kind_;
        result.className = className_;
        result.targetId = targetId_;
        result.title = title_;
        result.schema = schemaNames_;
        for (const std::string& name : order_) result.callbacks.push_back(name);
        result.issues = issues_;
        result.fallbacks = fallbacks_;
        return result;
    }

private:
    const std::string& src_;
    HaxeLuaOptions opt_;
    HxScript script_;
    bool codename_ = false;
    std::string className_;
    std::string uniq_;
    std::string kind_;
    std::string targetId_;
    std::string title_;
    std::vector<std::string> schemaNames_;
    std::vector<std::string> schemaTypes_;
    std::map<std::string, std::string> imports_;
    std::vector<Issue> issues_;
    std::set<std::string> issueKeys_;
    int fallbacks_ = 0;
    int currentLine_ = 0;

    std::map<std::string, Sym> fields_;
    std::vector<std::string> fieldOrder_;
    std::map<std::string, std::string> fieldInit_;
    std::vector<std::pair<std::string, HxPtr>> lateInit_;
    std::map<std::string, std::string> methods_;
    std::vector<std::map<std::string, Sym>> scopes_;
    std::set<std::string> helpers_;
    std::set<std::string> varModeFields_;
    std::set<std::string> usedTags_;

    std::map<std::string, Callback> callbacks_;
    std::vector<std::string> order_;
    std::vector<std::string> methodText_;

    std::vector<std::string>* pre_ = nullptr;
    std::string wantTag_;
    int loopDepth_ = 0;
    int counter_ = 0;
    bool inCreate_ = false;
    bool cancelable_ = false;
    bool cancelUsed_ = false;
    bool inCallback_ = false;
    std::vector<std::string> continueLabels_;
    std::vector<bool> continueUsed_;
    std::map<std::string, std::string> eventMap_;
    std::string eventKind_;
    int mustPress_ = -1;
    std::map<std::string, int> valueIndex_;
    bool tweens_ = false;
    bool tweenEvents_ = false;
    bool timers_ = false;
    bool usesSelf_ = false;
    std::map<std::string, std::string> zIndex_;

    static std::string baseName(const std::string& file) {
        std::string name = file;
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        size_t dot = name.find('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        return name;
    }

    static std::string sanitize(const std::string& text) {
        std::string out;
        for (char c : text) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "s" + out;
        return out;
    }

    static std::string safe(const std::string& name) {
        if (kLuaKeywords.count(name)) return name + "_";
        return name;
    }

    void issue(int line, const std::string& message) {
        if (line == 0) line = currentLine_;
        std::string key = std::to_string(line) + message;
        if (!issueKeys_.insert(key).second) return;
        issues_.push_back({line, message});
    }

    std::string detectKind() {
        if (!script_.hasClass) return "script";
        std::string parent = lastSegment(script_.parent);
        if (parent == "Module") return "module";
        if (parent == "Song") return "song";
        if (parent == "Stage") return "stage";
        if (parent == "SongEvent") return "event";
        if (parent == "NoteKind") return "notekind";
        if (parent == "BaseCharacter" || parent == "SparrowCharacter" || parent == "MultiSparrowCharacter" || parent == "PackerCharacter" ||
            parent == "AnimateAtlasCharacter" || parent == "MultiAnimateAtlasCharacter")
            return "character";
        return "other";
    }

    std::string detectTarget() {
        for (const HxMember& m : script_.members) {
            if (m.name != "new" || !m.isFunction) continue;
            for (const HxPtr& s : m.body) {
                if (s->k == HxK::Call && s->a && s->a->k == HxK::Super && !s->list.empty() && s->list[0]->k == HxK::Str) return s->list[0]->s;
            }
        }
        return "";
    }

    void walk(const HxPtr& n, const std::function<void(const HxPtr&)>& fn) {
        if (!n) return;
        fn(n);
        walk(n->a, fn);
        walk(n->b, fn);
        walk(n->c, fn);
        for (const HxPtr& c : n->list) walk(c, fn);
        for (const HxStrPart& p : n->parts) walk(p.expr, fn);
        for (const HxCase& c : n->cases) {
            for (const HxPtr& v : c.values) walk(v, fn);
            walk(c.guard, fn);
            for (const HxPtr& b : c.body) walk(b, fn);
        }
        for (const HxCatch& c : n->catches) for (const HxPtr& b : c.body) walk(b, fn);
        for (const HxParam& p : n->params) walk(p.value, fn);
    }

    void walkMembers(const std::function<void(const HxPtr&)>& fn) {
        for (const HxMember& m : script_.members) {
            walk(m.value, fn);
            for (const HxPtr& s : m.body) walk(s, fn);
        }
        for (const HxPtr& s : script_.topLevel) walk(s, fn);
    }

    void readEventInfo() {
        for (const HxMember& m : script_.members) {
            if (m.name == "getTitle") {
                for (const HxPtr& s : m.body) {
                    if (s->k == HxK::Return && s->a && s->a->k == HxK::Str) title_ = s->a->s;
                }
            }
            if (m.name == "getEventSchema") {
                for (const HxPtr& s : m.body) {
                    walk(s, [&](const HxPtr& n) {
                        if (n->k != HxK::ObjLit) return;
                        std::string name;
                        std::string type;
                        for (size_t i = 0; i < n->keys.size(); i++) {
                            if (n->keys[i] == "name" && n->list[i]->k == HxK::Str) name = n->list[i]->s;
                            if (n->keys[i] == "type" && n->list[i]->k == HxK::Str) type = n->list[i]->s;
                        }
                        if (!name.empty()) {
                            schemaNames_.push_back(name);
                            schemaTypes_.push_back(type);
                        }
                    });
                }
            }
        }
        for (size_t i = 0; i < schemaNames_.size(); i++) valueIndex_[schemaNames_[i]] = static_cast<int>(i);
    }

    static bool typeIsSprite(const std::string& type) {
        std::string t = lastSegment(type.substr(0, type.find('<')));
        return t == "FlxSprite" || t == "FunkinSprite" || t == "FlxText" || t == "BGSprite" || t == "FlxAnimate" || t == "FunkinText" ||
               t == "FlxSkewedSprite" || t == "Bopper";
    }

    bool isCreation(const HxPtr& n) {
        if (!n) return false;
        if (n->k == HxK::Paren) return isCreation(n->a);
        if (n->k == HxK::New) {
            std::string t = lastSegment(n->s);
            return t == "FlxSprite" || t == "FunkinSprite" || t == "FlxText" || t == "FlxSkewedSprite" || t == "BGSprite";
        }
        if (n->k == HxK::Call && n->a && n->a->k == HxK::Field) {
            const HxPtr& callee = n->a;
            if (callee->a && callee->a->k == HxK::Ident && callee->a->s == "FunkinSprite") return true;
            static const std::set<std::string> fluent = {"loadGraphic", "loadTexture", "makeGraphic", "screenCenter", "loadFrames", "loadSparrow", "makeSolidColor"};
            if (fluent.count(callee->s)) return isCreation(callee->a);
        }
        return false;
    }

    void prepass() {
        std::map<std::string, int> plainAssign;
        walkMembers([&](const HxPtr& n) {
            if (n->k == HxK::Assign && n->s == "=") {
                const HxPtr& t = n->a;
                std::string name;
                if (t->k == HxK::Ident) name = t->s;
                if (t->k == HxK::Field && t->a && t->a->k == HxK::This) name = t->s;
                if (!name.empty() && !isCreation(n->b)) plainAssign[name]++;
            }
        });
        for (const HxMember& m : script_.members) {
            if (m.isFunction) {
                methods_[m.name] = kPsychApi.count(m.name) || kLuaKeywords.count(m.name) ? "hx_" + m.name : m.name;
                continue;
            }
            Sym s;
            s.lua = safe(m.name);
            if (kPsychApi.count(s.lua)) s.lua = "hx_" + s.lua;
            bool sprite = typeIsSprite(m.type) || isCreation(m.value);
            if (sprite) {
                s.spr = true;
                s.text = lastSegment(m.type) == "FlxText" || (m.value && m.value->k == HxK::New && lastSegment(m.value->s) == "FlxText");
                if (plainAssign.count(m.name)) {
                    varModeFields_.insert(m.name);
                    s.lit = false;
                } else {
                    s.lit = true;
                    s.lua = luaStr(m.name);
                    usedTags_.insert(m.name);
                }
            }
            applyType(s, m.type, m.value);
            fields_[m.name] = s;
            fieldOrder_.push_back(m.name);
        }
        std::set<std::string> spriteLocals;
        walkMembers([&](const HxPtr& n) {
            if (n->k == HxK::Var) {
                for (size_t i = 0; i < n->names.size(); i++) if (isCreation(n->list[i]) || typeIsSprite(n->keys[i])) spriteLocals.insert(n->names[i]);
            }
        });
        walkMembers([&](const HxPtr& n) {
            if (n->k == HxK::Call && n->a && n->a->k == HxK::Field && n->a->s == "push" && n->a->a && n->a->a->k == HxK::Ident && !n->list.empty()) {
                const HxPtr& arg = n->list[0];
                bool sprite = isCreation(arg);
                if (arg->k == HxK::Ident) {
                    auto f = fields_.find(arg->s);
                    if (f != fields_.end() && f->second.spr) sprite = true;
                    if (spriteLocals.count(arg->s)) sprite = true;
                }
                auto f = fields_.find(n->a->a->s);
                if (sprite && f != fields_.end()) {
                    f->second.sprArr = true;
                    f->second.arr = true;
                }
            }
        });
    }

    void applyType(Sym& s, const std::string& type, const HxPtr& value) {
        std::string base = lastSegment(type.substr(0, type.find('<')));
        if (base == "String") s.str = true;
        if (base == "Array") {
            s.arr = true;
            size_t lt = type.find('<');
            if (lt != std::string::npos && typeIsSprite(type.substr(lt + 1))) s.sprArr = true;
        }
        if (base == "Map" || base == "StringMap" || base == "IntMap" || base == "ObjectMap") s.map = true;
        if (base == "FlxTween") s.kind = "tween";
        if (base == "FlxTimer") s.kind = "timer";
        if (base == "FlxSound" || base == "FunkinSound") s.kind = "sound";
        if (value) {
            if (value->k == HxK::Str) s.str = true;
            if (value->k == HxK::ArrayLit) s.arr = true;
            if (value->k == HxK::MapLit) s.map = true;
            if (value->k == HxK::New && (lastSegment(value->s) == "Map" || lastSegment(value->s) == "StringMap" || lastSegment(value->s) == "IntMap")) s.map = true;
        }
    }

    Sym* findSym(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    Sym& declare(const std::string& name) {
        Sym& s = scopes_.back()[name];
        s = Sym();
        s.lua = safe(name);
        if (kPsychApi.count(s.lua)) s.lua = "hx_" + s.lua;
        return s;
    }

    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }

    std::string useHelper(const std::string& name) {
        helpers_.insert(name);
        if (name == "hx_contains" || name == "hx_lastIndexOf") helpers_.insert("hx_indexOf");
        return name;
    }

    std::string uidTag(const std::string& base) {
        useHelper("hx_uid");
        return luaStr(base) + " .. hx_uid()";
    }

    std::string newTag(const std::string& prefix) {
        counter_++;
        std::string base = prefix + "_" + uniq_ + "_" + std::to_string(counter_);
        if (loopDepth_ > 0 || !inCreate_) return uidTag(base + "_");
        return luaStr(base);
    }

    void emitPre(const std::string& line) {
        if (pre_) pre_->push_back(line);
        else throw Unsupported{"this code has to run inside a function"};
    }

    std::string segsExpr(const std::vector<Seg>& segs, size_t from, bool stripDot) {
        std::vector<std::string> parts;
        std::string literal;
        bool haveLiteral = false;
        for (size_t i = from; i < segs.size(); i++) {
            std::string text = segs[i].text;
            if (i == from && stripDot && segs[i].lit && !text.empty() && text[0] == '.') text = text.substr(1);
            if (segs[i].lit) {
                literal += text;
                haveLiteral = true;
            } else {
                if (haveLiteral) parts.push_back(luaStr(literal));
                literal.clear();
                haveLiteral = false;
                parts.push_back(text);
            }
        }
        if (haveLiteral) parts.push_back(luaStr(literal));
        if (parts.empty()) return "''";
        std::string out;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) out += " .. ";
            out += parts[i];
        }
        return out;
    }

    std::string pathExpr(const Val& v) { return segsExpr(v.segs, 0, v.k != VK::Obj); }

    std::string tagExpr(const Val& v) { return segsExpr(v.segs, 0, false).substr(0); }

    std::string joinPath(const Val& v, const std::string& suffix) {
        std::vector<Seg> segs = v.segs;
        segs.push_back({true, suffix});
        return segsExpr(segs, 0, false);
    }

    std::string tagOf(const Val& v) {
        if (v.k == VK::Obj && !v.segs.empty()) {
            Val t = v;
            t.segs.resize(1);
            return segsExpr(t.segs, 0, false);
        }
        if (v.k == VK::Lua && v.spr) return v.lua;
        throw Unsupported{"this value is not a sprite made by the script"};
    }

    bool subIs(const Val& v, const std::string& sub) {
        std::string joined;
        for (size_t i = 1; i < v.segs.size(); i++) {
            if (!v.segs[i].lit) return false;
            joined += v.segs[i].text;
        }
        return joined == sub;
    }

    std::string literalTag(const Val& v) {
        if (v.k == VK::Obj && !v.segs.empty() && v.segs[0].lit) return v.segs[0].text;
        return "";
    }

    Val asObj(const Val& v) {
        if (v.k == VK::Obj) return v;
        if (v.k == VK::Lua && v.spr) {
            Val o = objExpr(v.lua);
            o.text = v.text;
            return o;
        }
        return v;
    }

    std::string rd(const Val& v, int prec = 0) {
        std::string s;
        int p = P_ATOM;
        switch (v.k) {
            case VK::Lua:
                if (v.none) throw Unsupported{"this call has no value"};
                s = v.lua;
                p = v.prec;
                break;
            case VK::Obj:
                if (v.segs.size() == 1) s = tagExpr(v);
                else s = "getProperty(" + pathExpr(v) + ")";
                break;
            case VK::Game:
                if (v.segs.empty()) throw Unsupported{"PlayState itself can't be passed around in Lua"};
                s = "getProperty(" + pathExpr(v) + ")";
                break;
            case VK::Cls:
                if (v.segs.empty()) throw Unsupported{"class " + v.cls + " can't be used as a value in Lua"};
                s = "getPropertyFromClass(" + luaStr(v.cls) + ", " + pathExpr(v) + ")";
                break;
            case VK::Ns:
                throw Unsupported{v.cls + " can't be used as a value in Lua"};
            case VK::Event:
                return eventRead(v, prec);
            case VK::Stage:
                throw Unsupported{"the stage itself can't be used as a value in Lua"};
        }
        return p < prec ? "(" + s + ")" : s;
    }

    std::string L(const HxPtr& n, int prec = 0) { return rd(ev(n), prec); }

    std::string prefix(const Val& v) {
        std::string s = rd(v, P_ATOM);
        if (!s.empty() && (s[0] == '\'' || s[0] == '"' || s[0] == '{' || s.compare(0, 8, "function") == 0)) return "(" + s + ")";
        return s;
    }

    std::string argList(const std::vector<HxPtr>& args, size_t from = 0) {
        std::string out;
        for (size_t i = from; i < args.size(); i++) {
            if (i > from) out += ", ";
            out += L(args[i]);
        }
        return out;
    }

    std::string arg(const std::vector<HxPtr>& args, size_t i, const std::string& fallback) { return i < args.size() ? L(args[i]) : fallback; }

    std::string colorHex(const HxPtr& n) {
        Val v = ev(n);
        if (!v.hex.empty()) return luaStr(v.hex);
        if (v.str) return rd(v);
        return useHelper("hx_hex") + "(" + rd(v) + ")";
    }

    std::string colorNumber(const HxPtr& n) {
        Val v = ev(n);
        if (!v.hex.empty()) return "getColorFromHex(" + luaStr(v.hex) + ")";
        if (v.str) return "getColorFromHex(" + rd(v) + ")";
        return rd(v);
    }

    std::string cameraName(const Val& v) {
        std::string t = literalTag(v);
        if (v.segs.size() != 1) return "";
        if (t == "camGame") return "game";
        if (t == "camHUD") return "hud";
        if (t == "camOther") return "other";
        return "";
    }

    Val eventVal() {
        Val v;
        v.k = VK::Event;
        return v;
    }

    static bool atomic(const std::string& code) {
        if (code.empty() || !(std::isalpha(static_cast<unsigned char>(code[0])) || code[0] == '_') || code.back() != ')') return false;
        size_t open = code.find('(');
        if (open == std::string::npos) return false;
        int depth = 0;
        for (size_t i = open; i < code.size(); i++) {
            char c = code[i];
            if (c == '\'' || c == '"') {
                char q = c;
                i++;
                while (i < code.size() && code[i] != q) i += code[i] == '\\' ? 2 : 1;
                continue;
            }
            if (c == '(') depth++;
            if (c == ')' && --depth == 0) return i + 1 == code.size();
        }
        return false;
    }

    std::string eventRead(const Val& v, int prec) {
        auto f = eventMap_.find(v.path);
        if (f != eventMap_.end()) {
            bool simple = f->second.find(' ') == std::string::npos || atomic(f->second);
            return simple || prec == 0 ? f->second : "(" + f->second + ")";
        }
        std::string path = v.path;
        std::string field;
        size_t dot = path.rfind('.');
        std::string head = dot == std::string::npos ? "" : path.substr(0, dot);
        field = dot == std::string::npos ? path : path.substr(dot + 1);
        if ((head == "eventData.value" || head == "value") && (eventKind_ == "event" || eventKind_ == "handle")) return valueRead(field, prec);
        throw Unsupported{"event." + v.path + " has no Psych equivalent"};
    }

    std::string valueRead(const std::string& field, int prec) {
        auto it = valueIndex_.find(field);
        int index;
        if (it == valueIndex_.end()) {
            index = static_cast<int>(valueIndex_.size());
            valueIndex_[field] = index;
        } else {
            index = it->second;
        }
        if (index > 1) {
            issue(0, "Psych events only have two values; \"" + field + "\" is nil.");
            return "nil";
        }
        std::string name = index == 0 ? "value1" : "value2";
        std::string type = static_cast<size_t>(index) < schemaTypes_.size() ? schemaTypes_[index] : "";
        if (type == "float" || type == "integer" || type == "number") return "tonumber(" + name + ")";
        if (type == "bool") return prec > P_CMP ? "(" + name + " == 'true')" : name + " == 'true'";
        return name;
    }

    Val ident(const std::string& name, int line) {
        if (Sym* s = findSym(name)) return symVal(*s);
        auto f = fields_.find(name);
        if (f != fields_.end()) return symVal(f->second);
        auto m = methods_.find(name);
        if (m != methods_.end()) return lua(m->second);
        return global(name, line);
    }

    Val symVal(const Sym& s) {
        if (s.kind == "event") return eventVal();
        if (s.kind == "self") return objExpr("hx_self");
        if (s.kind == "timerParam" || s.kind == "tweenParam") {
            Val v = ns(s.kind);
            v.path = s.extra;
            v.lua = s.lua;
            return v;
        }
        if (s.spr) {
            if (s.lit) {
                Val v = objLit(s.lua.substr(1, s.lua.size() - 2));
                v.text = s.text;
                return v;
            }
            Val v = objExpr(s.lua);
            v.text = s.text;
            return v;
        }
        Val v = lua(s.lua);
        v.str = s.str;
        v.arr = s.arr;
        v.map = s.map;
        v.sprArr = s.sprArr;
        v.tagKind = s.kind;
        return v;
    }

    std::string importOf(const std::string& name) {
        auto it = imports_.find(name);
        return it == imports_.end() ? "" : it->second;
    }

    Val global(const std::string& name, int line) {
        static const std::set<std::string> namespaces = {"FlxG", "Paths", "Math", "Std", "StringTools", "FlxTween", "FlxEase", "FlxTimer", "FlxColor",
            "FlxMath", "FunkinSprite", "FlxSprite", "FlxText", "Highscore", "FunkinSound", "FlxSound", "FlxAxes", "FlxTweenType", "CountdownStep",
            "Reflect", "PlayState", "Conductor", "Preferences", "Options", "VideoCutscene", "Json", "Lambda", "Type", "FlxPoint", "Save"};
        if (name == "Preferences" || name == "Options" || name == "ClientPrefs") return ns("prefs");
        if (namespaces.count(name)) return ns(name);
        if (codename_) {
            Val g = codenameGlobal(name);
            if (g.k != VK::Lua || !g.lua.empty()) return g;
        }
        if (kind_ == "stage" && (name == "camZoom" || name == "defaultCameraZoom")) return gameProp("defaultCamZoom");
        if (kind_ == "character" && !name.empty() && std::islower(static_cast<unsigned char>(name[0]))) {
            usesSelf_ = true;
            Val self = objExpr("hx_self");
            return field(self, name, line);
        }
        if (opt_.stageProps.count(name)) return objLit(name);
        std::string imp = importOf(name);
        if (!imp.empty()) return clsVal(psychClass(imp));
        if (!name.empty() && std::isupper(static_cast<unsigned char>(name[0]))) throw Unsupported{"class " + name + " has no Lua equivalent"};
        issue(line, "\"" + name + "\" is not declared in this script; it is read as a Lua global.");
        return lua(safe(name));
    }

    static std::string psychClass(const std::string& path) {
        static const std::map<std::string, std::string> renames = {{"funkin.play.PlayState", "states.PlayState"}, {"funkin.Conductor", "backend.Conductor"},
            {"funkin.Paths", "backend.Paths"}, {"funkin.graphics.FunkinSprite", "flixel.FlxSprite"}, {"funkin.Preferences", "backend.ClientPrefs"},
            {"funkin.backend.system.Conductor", "backend.Conductor"}, {"funkin.game.PlayState", "states.PlayState"}};
        auto it = renames.find(path);
        return it == renames.end() ? path : it->second;
    }

    Val codenameGlobal(const std::string& name) {
        static const std::map<std::string, std::string> globals = {{"curBeat", "curBeat"}, {"curStep", "curStep"}, {"curMeasure", "curSection"},
            {"curSection", "curSection"}, {"downscroll", "downscroll"}, {"isStoryMode", "isStoryMode"}};
        auto g = globals.find(name);
        if (g != globals.end()) return numVal(g->second);
        if (name == "curBeatFloat") return numVal("getSongPosition() / crochet", P_MUL);
        if (name == "curStepFloat") return numVal("getSongPosition() / stepCrochet", P_MUL);
        if (name == "game") return gameRoot();
        if (name == "SONG") return ns("song");
        if (name == "stage") return ns("cnestage");
        static const std::set<std::string> gameNames = {"boyfriend", "dad", "gf", "girlfriend", "camGame", "camHUD", "camOther", "health", "misses",
            "combo", "songScore", "defaultCamZoom", "camZooming", "iconP1", "iconP2", "healthBar", "healthBarBG", "scoreTxt", "missesTxt",
            "accuracyTxt", "vocals", "inCutscene", "camFollow", "camZoomingInterval", "camZoomingStrength", "songLength", "accuracy",
            "maxHealth", "paused", "startingSong", "endingSong"};
        if (gameNames.count(name)) return gameField(gameRoot(), name);
        if (!name.empty() && std::islower(static_cast<unsigned char>(name[0])) && name != "trace") {
            issue(0, "\"" + name + "\" is read from PlayState with getProperty(); check it exists in Psych.");
            return gameProp(name);
        }
        return lua("");
    }

    Val gameField(const Val&, const std::string& name) {
        static const std::map<std::string, std::string> objects = {{"camGame", "camGame"}, {"camHUD", "camHUD"}, {"camCutscene", "camOther"},
            {"camOther", "camOther"}, {"iconP1", "iconP1"}, {"iconP2", "iconP2"}, {"healthBar", "healthBar"}, {"healthBarBG", "healthBar.bg"},
            {"scoreText", "scoreTxt"}, {"scoreTxt", "scoreTxt"}, {"timeBar", "timeBar"}, {"timeTxt", "timeTxt"}, {"botplayTxt", "botplayTxt"},
            {"vocals", "vocals"}, {"camFollow", "camFollow"}, {"cameraFollowPoint", "camFollow"}, {"boyfriend", "boyfriend"}, {"dad", "dad"},
            {"gf", "gf"}, {"girlfriend", "gf"}, {"missesTxt", "scoreTxt"}, {"accuracyTxt", "scoreTxt"}};
        static const std::map<std::string, std::string> props = {{"currentCameraZoom", "defaultCamZoom"}, {"defaultCameraZoom", "defaultCamZoom"},
            {"defaultCamZoom", "defaultCamZoom"}, {"stageZoom", "defaultCamZoom"}, {"health", "health"}, {"songScore", "songScore"},
            {"misses", "songMisses"}, {"songMisses", "songMisses"}, {"combo", "combo"}, {"camZooming", "camZooming"}, {"isInCutscene", "inCutscene"},
            {"inCutscene", "inCutscene"}, {"isBotPlayMode", "cpuControlled"}, {"cpuControlled", "cpuControlled"}, {"isGamePaused", "paused"},
            {"paused", "paused"}, {"startingSong", "startingSong"}, {"endingSong", "endingSong"}, {"songLength", "songLength"},
            {"maxHealth", "maxHealth"}, {"accuracy", "ratingPercent"}, {"camZoomingInterval", "camZoomingMult"}, {"camZoomingStrength", "camZoomingDecay"},
            {"cameraBopMultiplier", "camZoomingMult"}};
        if (name == "currentStage") {
            Val v;
            v.k = VK::Stage;
            return v;
        }
        if (name == "currentSong" || name == "SONG") return ns("song");
        if (name == "currentDifficulty") return strVal("difficultyName");
        if (name == "instance") return gameRoot();
        auto o = objects.find(name);
        if (o != objects.end()) return objLit(o->second);
        auto p = props.find(name);
        if (p != props.end()) {
            Val v = gameProp(p->second);
            return v;
        }
        return gameProp(name);
    }

    Val conductorField(const std::string& name) {
        if (name == "instance") return ns("conductor");
        if (name == "currentBeat" || name == "curBeat") return numVal("curBeat");
        if (name == "currentStep" || name == "curStep") return numVal("curStep");
        if (name == "currentMeasure" || name == "curMeasure" || name == "curSection") return numVal("curSection");
        if (name == "songPosition") return numVal("getSongPosition()");
        if (name == "bpm") return numVal("bpm");
        if (name == "beatLengthMs" || name == "crochet") return numVal("crochet");
        if (name == "stepLengthMs" || name == "stepCrochet") return numVal("stepCrochet");
        if (name == "currentBeatTime" || name == "curBeatFloat") return numVal("getSongPosition() / crochet", P_MUL);
        if (name == "currentStepTime" || name == "curStepFloat") return numVal("getSongPosition() / stepCrochet", P_MUL);
        if (name == "currentMeasureTime" || name == "curMeasureFloat") return numVal("getSongPosition() / (crochet * 4)", P_MUL);
        Val v = clsVal("backend.Conductor");
        v.segs.push_back({true, "." + name});
        return v;
    }

    Val nsField(const Val& base, const std::string& name, int line) {
        const std::string& n = base.cls;
        if (n == "PlayState") {
            if (name == "instance") return gameRoot();
            if (name == "SONG") return ns("song");
            if (name == "isStoryMode") return lua("isStoryMode");
            if (name == "storyDifficulty") return numVal("difficulty");
            if (name == "seenCutscene") return lua("seenCutscene");
            Val v = clsVal("states.PlayState");
            v.segs.push_back({true, "." + name});
            return v;
        }
        if (n == "conductor" || n == "Conductor") return conductorField(name);
        if (n == "FlxG") {
            if (name == "camera") return objLit("camGame");
            if (name == "width") return numVal("screenWidth");
            if (name == "height") return numVal("screenHeight");
            if (name == "random" || name == "keys" || name == "mouse" || name == "sound") return ns(name);
            Val v = clsVal("flixel.FlxG");
            v.segs.push_back({true, "." + name});
            return v;
        }
        if (n == "keys") {
            if (name == "justPressed") return ns("keys:keyboardJustPressed");
            if (name == "pressed") return ns("keys:keyboardPressed");
            if (name == "justReleased") return ns("keys:keyboardReleased");
            throw Unsupported{"FlxG.keys." + name + " has no Lua equivalent"};
        }
        if (n.compare(0, 5, "keys:") == 0) return callVal(n.substr(5) + "(" + luaStr(name) + ")");
        if (n == "mouse") {
            if (name == "justPressed") return callVal("mouseClicked('left')");
            if (name == "pressed") return callVal("mousePressed('left')");
            if (name == "justReleased") return callVal("mouseReleased('left')");
            if (name == "justPressedRight") return callVal("mouseClicked('right')");
            if (name == "pressedRight") return callVal("mousePressed('right')");
            if (name == "x" || name == "screenX") return numVal("getMouseX('game')");
            if (name == "y" || name == "screenY") return numVal("getMouseY('game')");
            Val v = clsVal("flixel.FlxG");
            v.segs.push_back({true, ".mouse." + name});
            return v;
        }
        if (n == "sound") {
            Val v = clsVal("flixel.FlxG");
            v.segs.push_back({true, ".sound." + name});
            return v;
        }
        if (n == "Math") {
            if (name == "PI") return numVal("math.pi");
            if (name == "POSITIVE_INFINITY") return numVal("math.huge");
            if (name == "NEGATIVE_INFINITY") return numVal("-math.huge", P_UN);
            if (name == "NaN") return numVal("(0 / 0)");
            throw Unsupported{"Math." + name + " has no Lua equivalent"};
        }
        if (n == "FlxColor") return colorConst(name);
        if (n == "FlxEase") return strVal(luaStr(name));
        if (n == "FlxTweenType") {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return strVal(luaStr(lower));
        }
        if (n == "FlxAxes") {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return strVal(luaStr(lower));
        }
        if (n == "CountdownStep") {
            static const std::map<std::string, int> steps = {{"BEFORE", -1}, {"THREE", 0}, {"TWO", 1}, {"ONE", 2}, {"GO", 3}, {"AFTER", 4}};
            auto it = steps.find(name);
            if (it != steps.end()) return numVal(std::to_string(it->second));
        }
        if (n == "prefs") return prefsField(name);
        if (n == "Highscore") {
            if (name == "tallies") return ns("tallies");
            throw Unsupported{"Highscore." + name + " has no Lua equivalent"};
        }
        if (n == "tallies") {
            if (name == "missed") return numVal("getProperty('songMisses')");
            if (name == "combo") return numVal("getProperty('combo')");
            if (name == "totalNotesHit") return numVal("getProperty('songHits')");
            throw Unsupported{"Highscore.tallies." + name + " has no Lua equivalent"};
        }
        if (n == "song") {
            if (name == "id" || name == "songId" || name == "song") return strVal("songPath");
            if (name == "songName" || name == "name" || name == "title" || name == "displayName") return strVal("songName");
            if (name == "meta") return ns("song");
            if (name == "bpm") return numVal("bpm");
            if (name == "stage") return strVal("curStage");
            throw Unsupported{"song." + name + " has no Lua equivalent"};
        }
        if (n == "timerParam") {
            size_t a = base.path.find('|');
            size_t b = base.path.find('|', a + 1);
            std::string tag = base.path.substr(0, a);
            std::string loops = base.path.substr(a + 1, b - a - 1);
            std::string left = base.path.substr(b + 1);
            if (name == "loopsLeft") return numVal(left);
            if (name == "elapsedLoops") return numVal(loops + " - " + left, P_ADD);
            if (name == "finished") return lua(left + " == 0", P_CMP);
            if (name == "loops") return numVal(loops);
            throw Unsupported{"timer." + name + " has no Lua equivalent"};
        }
        (void)line;
        throw Unsupported{n + "." + name + " has no Lua equivalent"};
    }

    Val prefsField(const std::string& name) {
        static const std::map<std::string, std::string> globals = {{"downscroll", "downscroll"}, {"downScroll", "downscroll"},
            {"middlescroll", "middlescroll"}, {"middleScroll", "middlescroll"}, {"flashingLights", "flashingLights"}, {"flashing", "flashingLights"},
            {"lowQuality", "lowQuality"}, {"ghostTapping", "ghostTapping"}, {"zoomCamera", "cameraZoomOnBeat"}, {"camZoomOnBeat", "cameraZoomOnBeat"},
            {"shaders", "shadersEnabled"}, {"hideHud", "hideHud"}, {"framerate", "framerate"}, {"noteOffset", "noteOffset"}};
        auto it = globals.find(name);
        if (it != globals.end()) return lua(it->second);
        if (name == "naughtyness") return lua("true");
        Val v = clsVal("backend.ClientPrefs");
        v.segs.push_back({true, ".data." + name});
        return v;
    }

    Val colorConst(const std::string& name) {
        static const std::map<std::string, std::string> colors = {{"WHITE", "FFFFFF"}, {"BLACK", "000000"}, {"RED", "FF0000"}, {"GREEN", "008000"},
            {"LIME", "00FF00"}, {"BLUE", "0000FF"}, {"YELLOW", "FFFF00"}, {"ORANGE", "FFA500"}, {"PURPLE", "800080"}, {"PINK", "FFC0CB"},
            {"MAGENTA", "FF00FF"}, {"CYAN", "00FFFF"}, {"GRAY", "808080"}, {"BROWN", "8B4513"}, {"TRANSPARENT", "000000"}};
        auto it = colors.find(name);
        if (it == colors.end()) throw Unsupported{"FlxColor." + name + " has no Lua equivalent"};
        Val v = numVal("getColorFromHex(" + luaStr(it->second) + ")");
        v.hex = it->second;
        return v;
    }

    Val field(const Val& rawBase, const std::string& name, int line) {
        Val base = asObj(rawBase);
        switch (base.k) {
            case VK::Lua: {
                if (name == "length") {
                    Val v = numVal("#" + rd(base, P_UN), P_UN);
                    return v;
                }
                if (base.tagKind == "sound") {
                    if (name == "volume") return numVal("getSoundVolume(" + rd(base) + ")");
                    if (name == "time") return numVal("getSoundTime(" + rd(base) + ")");
                }
                if (isIdent(name) && !kLuaKeywords.count(name)) return lua(prefix(base) + "." + name);
                return lua(prefix(base) + "[" + luaStr(name) + "]");
            }
            case VK::Obj: {
                Val v = base;
                std::string f = name;
                if (f == "characterId") f = "curCharacter";
                if (f == "singTimeSteps") f = "singDuration";
                v.segs.push_back({true, "." + f});
                return v;
            }
            case VK::Game:
                if (base.segs.empty()) return gameField(base, name);
                {
                    Val v = base;
                    v.segs.push_back({true, "." + name});
                    return v;
                }
            case VK::Cls: {
                Val v = base;
                v.segs.push_back({true, "." + name});
                return v;
            }
            case VK::Ns:
                return nsField(base, name, line);
            case VK::Event: {
                Val v = base;
                v.path += v.path.empty() ? name : "." + name;
                return v;
            }
            case VK::Stage:
                throw Unsupported{"stage." + name + " has no Lua equivalent"};
        }
        return base;
    }

    Val ev(const HxPtr& n) {
        switch (n->k) {
            case HxK::Null: return lua("nil");
            case HxK::True: return lua("true");
            case HxK::False: return lua("false");
            case HxK::Int: {
                Val v = numVal(numText(n->num));
                if (n->s.size() > 2 && (n->s[1] == 'x' || n->s[1] == 'X')) {
                    std::string hex = n->s.substr(2);
                    for (char& c : hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (hex.size() >= 6) v.hex = hex.substr(hex.size() - 6);
                    if (hex.size() == 8) v.lua = "getColorFromHex(" + luaStr(v.hex) + ")";
                }
                return v;
            }
            case HxK::Float: return numVal(numText(n->num));
            case HxK::Str: return stringVal(n);
            case HxK::Regex: throw Unsupported{"regular expressions have no Lua equivalent"};
            case HxK::Ident: return ident(n->s, n->line);
            case HxK::This:
                if (kind_ == "character") {
                    usesSelf_ = true;
                    return objExpr("hx_self");
                }
                if (kind_ == "stage") {
                    Val v;
                    v.k = VK::Stage;
                    return v;
                }
                throw Unsupported{"\"this\" can't be passed around in Lua"};
            case HxK::Super: throw Unsupported{"super has no Lua equivalent"};
            case HxK::Field: {
                if (n->a->k == HxK::This) {
                    auto f = fields_.find(n->s);
                    if (f != fields_.end()) return symVal(f->second);
                    auto m = methods_.find(n->s);
                    if (m != methods_.end()) return lua(m->second);
                    return global(n->s, n->line);
                }
                if (n->a->k == HxK::Ident && n->a->s == "FlxG" && n->s == "save") throw Unsupported{"FlxG.save has no Lua equivalent"};
                return field(ev(n->a), n->s, n->line);
            }
            case HxK::Call: return call(n);
            case HxK::Index: return index(n);
            case HxK::New: return construct(n);
            case HxK::Unop: return unop(n);
            case HxK::Postfix: return increment(n, true);
            case HxK::Binop: return binop(n);
            case HxK::Assign: return assignExpr(n);
            case HxK::Ternary: {
                std::string c = L(n->a, P_AND);
                Val b = ev(n->b);
                Val e = ev(n->c);
                std::string bs = rd(b, P_AND);
                std::string es = rd(e, P_OR + 1);
                Val v;
                if (bs == "nil" || bs == "false") v = lua("(function() if " + L(n->a) + " then return " + rd(b) + " end return " + rd(e) + " end)()");
                else v = lua(c + " and " + bs + " or " + es, P_OR);
                v.str = b.str && e.str;
                return v;
            }
            case HxK::ArrayLit: {
                Val v = lua("{" + argList(n->list) + "}");
                v.arr = true;
                for (const HxPtr& item : n->list) if (isCreation(item)) v.sprArr = true;
                return v;
            }
            case HxK::MapLit: {
                std::string out = "{";
                for (size_t i = 0; i + 1 < n->list.size(); i += 2) {
                    if (i) out += ", ";
                    out += "[" + L(n->list[i]) + "] = " + L(n->list[i + 1]);
                }
                Val v = lua(out + "}");
                v.map = true;
                return v;
            }
            case HxK::ObjLit: {
                std::string out = "{";
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (i) out += ", ";
                    std::string key = n->keys[i];
                    out += (isIdent(key) && !kLuaKeywords.count(key) ? key : "[" + luaStr(key) + "]") + " = " + L(n->list[i]);
                }
                return lua(out + "}");
            }
            case HxK::Func: return lua(function(n->params, n->b, {}), 0);
            case HxK::Cast: return ev(n->a);
            case HxK::Paren: {
                Val v = ev(n->a);
                return v;
            }
            case HxK::If: {
                if (!n->c) throw Unsupported{"an if without else has no value"};
                std::string c = L(n->a);
                Val b = valueOfBranch(n->b);
                Val e = valueOfBranch(n->c);
                return lua("(function() if " + c + " then return " + rd(b) + " end return " + rd(e) + " end)()");
            }
            case HxK::Is: throw Unsupported{"\"is\" type checks have no Lua equivalent"};
            case HxK::Comprehension: return comprehension(n);
            default:
                throw Unsupported{"this expression has no Lua equivalent"};
        }
    }

    Val comprehension(const HxPtr& n) {
        HxPtr loop = n->a;
        if (!loop || loop->k != HxK::For) throw Unsupported{"this array comprehension has no Lua equivalent"};
        std::function<std::string(const HxPtr&)> leaf = [&](const HxPtr& b) -> std::string {
            if (b->k == HxK::Block && b->list.size() == 1) return leaf(b->list[0]);
            if (b->k == HxK::If && !b->c) return "if " + L(b->a) + " then " + leaf(b->b) + " end";
            if (b->k == HxK::For || b->k == HxK::Block || b->k == HxK::While) throw Unsupported{"nested comprehensions have no Lua equivalent"};
            return "hx_out[#hx_out + 1] = " + L(b);
        };
        HxPtr range = loop->a;
        while (range->k == HxK::Paren) range = range->a;
        pushScope();
        std::string head;
        std::string var = safe(loop->names[0]);
        bool spr = false;
        if (loop->names.size() == 1 && range->k == HxK::Binop && range->s == "...") {
            head = "for " + var + " = " + L(range->a) + ", " + L(range->b, P_ADD) + " - 1 do ";
        } else if (loop->names.size() == 1) {
            Val coll = ev(loop->a);
            if (coll.k != VK::Lua) {
                popScope();
                throw Unsupported{"only Lua tables can be looped over in Lua"};
            }
            spr = coll.sprArr;
            head = "for _, " + var + " in " + (coll.map ? "pairs(" : "ipairs(") + rd(coll) + ") do ";
        } else {
            popScope();
            throw Unsupported{"this array comprehension has no Lua equivalent"};
        }
        Sym& s = declare(loop->names[0]);
        s.lua = var;
        s.spr = spr;
        std::string inner;
        try {
            inner = leaf(loop->b);
        } catch (...) {
            popScope();
            throw;
        }
        popScope();
        Val v = lua("(function() local hx_out = {} " + head + inner + " end return hx_out end)()");
        v.arr = true;
        return v;
    }

    Val valueOfBranch(const HxPtr& n) {
        if (n->k == HxK::Block && n->list.size() == 1) return ev(n->list[0]);
        if (n->k == HxK::Block) throw Unsupported{"a block used as a value has no Lua equivalent"};
        return ev(n);
    }

    Val stringVal(const HxPtr& n) {
        if (n->parts.empty()) {
            Val v = strVal(luaStr(n->s));
            std::string hex;
            if ((n->s.size() == 7 && n->s[0] == '#') || n->s.size() == 6) {
                std::string h = n->s.size() == 7 ? n->s.substr(1) : n->s;
                bool ok = true;
                for (char c : h) if (!std::isxdigit(static_cast<unsigned char>(c))) ok = false;
                if (ok) {
                    for (char& c : h) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    v.hex = h;
                }
            }
            return v;
        }
        std::string out;
        for (const HxStrPart& p : n->parts) {
            std::string piece;
            if (p.isExpr) {
                Val v = ev(p.expr);
                piece = v.str ? rd(v, P_CAT + 1) : "tostring(" + rd(v) + ")";
            } else {
                if (p.text.empty()) continue;
                piece = luaStr(p.text);
            }
            if (!out.empty()) out += " .. ";
            out += piece;
        }
        if (out.empty()) out = "''";
        return strVal(out, P_CAT);
    }

    Val unop(const HxPtr& n) {
        if (n->s == "++" || n->s == "--") return increment(n, false);
        if (n->s == "!") return lua("not " + L(n->a, P_UN), P_UN);
        if (n->s == "-") {
            std::string inner = L(n->a, P_UN);
            // "--" starts a comment in Lua, so a negated negative needs parentheses.
            if (!inner.empty() && inner[0] == '-') inner = "(" + inner + ")";
            return numVal("-" + inner, P_UN);
        }
        return numVal("bit.bnot(" + L(n->a) + ")");
    }

    Val increment(const HxPtr& n, bool postfix) {
        std::string op = n->s == "++" ? "+" : "-";
        std::string target = L(n->a);
        Out o;
        assignTo(n->a, op == "+" ? "+=" : "-=", numVal("1"), o);
        std::string body;
        for (const std::string& l : o.lines) body += l + " ";
        if (postfix) return numVal("(function() local hx_v = " + target + " " + body + "return hx_v end)()");
        return numVal("(function() " + body + "return " + target + " end)()");
    }

    Val binop(const HxPtr& n) {
        const std::string& op = n->s;
        if (op == "+") {
            Val a = ev(n->a);
            Val b = ev(n->b);
            if (a.str || b.str) {
                std::string as = a.str || a.isNum ? rd(a, P_CAT + 1) : "tostring(" + rd(a) + ")";
                if (a.str && a.prec == P_CAT) as = rd(a, P_CAT);
                std::string bs = b.str || b.isNum ? rd(b, P_CAT) : "tostring(" + rd(b) + ")";
                return strVal(as + " .. " + bs, P_CAT);
            }
            return numVal(rd(a, P_ADD) + " + " + rd(b, P_ADD + 1), P_ADD);
        }
        if (op == "-") return numVal(L(n->a, P_ADD) + " - " + L(n->b, P_ADD + 1), P_ADD);
        if (op == "*") return numVal(L(n->a, P_MUL) + " * " + L(n->b, P_MUL + 1), P_MUL);
        if (op == "/") return numVal(L(n->a, P_MUL) + " / " + L(n->b, P_MUL + 1), P_MUL);
        if (op == "%") return numVal(L(n->a, P_MUL) + " % " + L(n->b, P_MUL + 1), P_MUL);
        if (op == "==" || op == "!=") {
            bool eq = op == "==";
            if (n->b->k == HxK::Null || n->a->k == HxK::Null) {
                const HxPtr& other = n->b->k == HxK::Null ? n->a : n->b;
                Val v = asObj(ev(other));
                if ((v.k == VK::Game && v.segs.empty()) || v.k == VK::Stage || (v.k == VK::Ns && (v.cls == "conductor" || v.cls == "song"))) return lua(eq ? "false" : "true");
                if (v.k == VK::Obj && v.segs.size() == 1) {
                    std::string check;
                    if (v.segs[0].lit && usedTags_.count(v.segs[0].text)) check = "luaSpriteExists(" + tagExpr(v) + ")";
                    else if (!v.segs[0].lit) check = tagExpr(v) + " ~= nil and luaSpriteExists(" + tagExpr(v) + ")";
                    else check = "getProperty(" + luaStr(v.segs[0].text + ".alpha") + ") ~= nil";
                    if (eq) return lua("not (" + check + ")", P_UN);
                    return lua(check, check.find(" and ") != std::string::npos ? P_AND : P_CMP);
                }
                return lua(rd(v, P_CMP + 1) + (eq ? " == nil" : " ~= nil"), P_CMP);
            }
            return lua(L(n->a, P_CMP + 1) + (eq ? " == " : " ~= ") + L(n->b, P_CMP + 1), P_CMP);
        }
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return lua(L(n->a, P_CMP + 1) + " " + op + " " + L(n->b, P_CMP + 1), P_CMP);
        if (op == "&&") return lua(L(n->a, P_AND) + " and " + L(n->b, P_AND + 1), P_AND);
        if (op == "||") return lua(L(n->a, P_OR) + " or " + L(n->b, P_OR + 1), P_OR);
        if (op == "??") return lua(useHelper("hx_coalesce") + "(" + L(n->a) + ", " + L(n->b) + ")");
        static const std::map<std::string, std::string> bits = {{"&", "bit.band"}, {"|", "bit.bor"}, {"^", "bit.bxor"}, {"<<", "bit.lshift"},
            {">>", "bit.arshift"}, {">>>", "bit.rshift"}};
        auto b = bits.find(op);
        if (b != bits.end()) return numVal(b->second + "(" + L(n->a) + ", " + L(n->b) + ")");
        throw Unsupported{"the " + op + " operator has no Lua equivalent here"};
    }

    Val index(const HxPtr& n) {
        Val base = ev(n->a);
        if (base.k == VK::Event) {
            if (n->b->k == HxK::Int) {
                base.path += "[" + numText(n->b->num) + "]";
                return base;
            }
            throw Unsupported{"a computed event index has no Psych equivalent"};
        }
        base = asObj(base);
        if (base.k == VK::Obj || base.k == VK::Game || base.k == VK::Cls) {
            Val v = base;
            if (n->b->k == HxK::Int) v.segs.push_back({true, "[" + numText(n->b->num) + "]"});
            else {
                v.segs.push_back({true, "["});
                v.segs.push_back({false, L(n->b)});
                v.segs.push_back({true, "]"});
            }
            return v;
        }
        if (base.k != VK::Lua) throw Unsupported{"this index has no Lua equivalent"};
        Val idx = ev(n->b);
        std::string key;
        if (base.map || idx.str) key = rd(idx);
        else if (n->b->k == HxK::Int) key = numText(n->b->num + 1);
        else key = rd(idx, P_ADD) + " + 1";
        Val v = lua(prefix(base) + "[" + key + "]");
        if (base.sprArr) v.spr = true;
        return v;
    }

    std::string takeTag(const std::string& prefix) {
        if (!wantTag_.empty()) {
            std::string t = wantTag_;
            wantTag_.clear();
            return t;
        }
        return newTag(prefix);
    }

    Val makeTagValue(const std::string& tag) {
        if (tag.size() >= 2 && tag.front() == '\'' && tag.back() == '\'' && tag.find(" .. ") == std::string::npos) {
            Val v = objLit(tag.substr(1, tag.size() - 2));
            return v;
        }
        if (isIdent(tag)) return objExpr(tag);
        throw Unsupported{"internal: tag expression must be stored first"};
    }

    std::string storedTag(const std::string& prefix) {
        std::string tag = takeTag(prefix);
        if (tag.find(" .. ") != std::string::npos) {
            counter_++;
            std::string name = "hx_t" + std::to_string(counter_);
            emitPre("local " + name + " = " + tag);
            return name;
        }
        return tag;
    }

    Val construct(const HxPtr& n) {
        std::string t = lastSegment(n->s);
        const std::vector<HxPtr>& a = n->list;
        if (t == "FlxSprite" || t == "FlxSkewedSprite") {
            std::string tag = storedTag("hxSprite");
            std::string image = a.size() > 2 ? L(a[2]) : "''";
            emitPre("makeLuaSprite(" + tag + ", " + image + ", " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ")");
            return makeTagValue(tag);
        }
        if (t == "FunkinSprite") {
            std::string tag = storedTag("hxSprite");
            emitPre("makeLuaSprite(" + tag + ", '', " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ")");
            return makeTagValue(tag);
        }
        if (t == "FlxText" || t == "FunkinText") {
            std::string tag = storedTag("hxText");
            emitPre("makeLuaText(" + tag + ", " + arg(a, 3, "''") + ", " + arg(a, 2, "0") + ", " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ")");
            if (a.size() > 4) emitPre("setTextSize(" + tag + ", " + L(a[4]) + ")");
            Val v = makeTagValue(tag);
            v.text = true;
            return v;
        }
        if (t == "BGSprite") {
            std::string tag = storedTag("hxSprite");
            bool animated = a.size() > 5 && a[5]->k != HxK::Null;
            emitPre(std::string(animated ? "makeAnimatedLuaSprite(" : "makeLuaSprite(") + tag + ", " + arg(a, 0, "''") + ", " + arg(a, 1, "0") + ", " +
                    arg(a, 2, "0") + ")");
            if (a.size() > 4) emitPre("setScrollFactor(" + tag + ", " + L(a[3]) + ", " + L(a[4]) + ")");
            return makeTagValue(tag);
        }
        if (t == "FlxTimer") {
            std::string tag = storedTag("hxTimer");
            Val v = lua(tag);
            v.tagKind = "timer";
            return v;
        }
        if (t == "Map" || t == "StringMap" || t == "IntMap" || t == "ObjectMap") {
            Val v = lua("{}");
            v.map = true;
            return v;
        }
        if (t == "Array") {
            Val v = lua("{}");
            v.arr = true;
            return v;
        }
        if (t == "FlxPoint") {
            return lua("{x = " + arg(a, 0, "0") + ", y = " + arg(a, 1, "0") + "}");
        }
        throw Unsupported{"new " + t + "() has no Lua equivalent"};
    }

    Val creation(const std::string& method, const std::vector<HxPtr>& a) {
        std::string tag = storedTag("hxSprite");
        if (method == "create") {
            emitPre("makeLuaSprite(" + tag + ", " + arg(a, 2, "''") + ", " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ")");
        } else if (method == "createSparrow") {
            emitPre("makeAnimatedLuaSprite(" + tag + ", " + arg(a, 2, "''") + ", " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ")");
        } else if (method == "createPacker") {
            emitPre("makeAnimatedLuaSprite(" + tag + ", " + arg(a, 2, "''") + ", " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ", 'packer')");
        } else if (method == "createTextureAtlas") {
            emitPre("makeFlxAnimateSprite(" + tag + ", " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ", " + arg(a, 2, "''") + ")");
        } else {
            throw Unsupported{"FunkinSprite." + method + "() has no Lua equivalent"};
        }
        return makeTagValue(tag);
    }

    Val call(const HxPtr& n) {
        const HxPtr& callee = n->a;
        const std::vector<HxPtr>& a = n->list;
        if (callee->k == HxK::Super) return noneVal();
        if (callee->k == HxK::Field && callee->a->k == HxK::Super) return noneVal();
        if (callee->k == HxK::Ident) return callIdent(callee->s, a, n->line);
        if (callee->k == HxK::Field) {
            if (callee->a->k == HxK::This) {
                if (methods_.count(callee->s)) return callIdent(callee->s, a, n->line);
                if (kind_ == "stage" || kind_ == "character") return callIdent(callee->s, a, n->line);
            }
            Val base = ev(callee->a);
            return method(base, callee->s, a, n->line);
        }
        Val f = ev(callee);
        return callVal(prefix(f) + "(" + argList(a) + ")");
    }

    Val callIdent(const std::string& name, const std::vector<HxPtr>& a, int line) {
        if (Sym* s = findSym(name)) {
            Val f = symVal(*s);
            if (f.k == VK::Lua) return callVal(prefix(f) + "(" + argList(a) + ")");
        }
        auto fieldFn = fields_.find(name);
        if (fieldFn != fields_.end() && !fieldFn->second.spr) return callVal(fieldFn->second.lua + "(" + argList(a) + ")");
        auto m = methods_.find(name);
        if (m != methods_.end()) return callVal(m->second + "(" + argList(a) + ")");
        if (name == "trace") {
            std::string out;
            for (size_t i = 0; i < a.size(); i++) {
                Val v = ev(a[i]);
                if (i) out += " .. ', ' .. ";
                out += v.str ? rd(v, a.size() == 1 ? 0 : P_CAT + 1) : "tostring(" + rd(v) + ")";
            }
            return callVal("debugPrint(" + (out.empty() ? "''" : out) + ")");
        }
        if (kind_ == "stage" && !codename_) {
            Val stage;
            stage.k = VK::Stage;
            static const std::set<std::string> stageMethods = {"getNamedProp", "getBoyfriend", "getDad", "getGirlfriend", "add", "remove", "refresh",
                "addProp", "addBopper", "insert", "getCharacter"};
            if (stageMethods.count(name)) return method(stage, name, a, line);
        }
        if (kind_ == "character" && !codename_) {
            usesSelf_ = true;
            return method(objExpr("hx_self"), name, a, line);
        }
        if (codename_) {
            if (name == "add") return addSprite(a, "true");
            if (name == "insert") {
                if (a.size() < 2) throw Unsupported{"insert() needs two arguments"};
                std::vector<HxPtr> rest(a.begin() + 1, a.end());
                return addSprite(rest, "false");
            }
            if (name == "remove") return removeSprite(a, false);
            if (name == "disableScript") return callVal("close()");
        }
        throw Unsupported{name + "() has no Lua equivalent"};
    }

    Val addSprite(const std::vector<HxPtr>& a, const std::string& front) {
        if (a.empty()) throw Unsupported{"add() needs a sprite"};
        Val v = asObj(ev(a[0]));
        if (v.k != VK::Obj || v.segs.size() != 1) throw Unsupported{"only sprites made by the script can be added from Lua"};
        std::string f = front;
        std::string t = literalTag(v);
        if (!t.empty() && zIndex_.count(t)) f = zIndex_[t];
        return callVal(std::string(v.text ? "addLuaText(" : "addLuaSprite(") + tagExpr(v) + (v.text ? "" : ", " + f) + ")");
    }

    Val removeSprite(const std::vector<HxPtr>& a, bool destroy) {
        if (a.empty()) throw Unsupported{"remove() needs a sprite"};
        Val v = asObj(ev(a[0]));
        if (v.k != VK::Obj || v.segs.size() != 1) throw Unsupported{"only sprites made by the script can be removed from Lua"};
        return callVal(std::string(v.text ? "removeLuaText(" : "removeLuaSprite(") + tagExpr(v) + ", " + (destroy ? "true" : "false") + ")");
    }

    std::string callArgs(const std::vector<HxPtr>& a) {
        std::string out = "{";
        for (size_t i = 0; i < a.size(); i++) {
            if (i) out += ", ";
            Val v = ev(a[i]);
            if (v.k == VK::Obj && v.segs.size() == 1) issue(a[i]->line, "a sprite passed to a Haxe method from Lua arrives as its tag name.");
            out += rd(v);
        }
        return out + "}";
    }

    Val method(const Val& rawBase, const std::string& name, const std::vector<HxPtr>& a, int line) {
        Val base = asObj(rawBase);
        switch (base.k) {
            case VK::Obj: return objMethod(base, name, a, line);
            case VK::Game: {
                if (base.segs.empty()) {
                    if (name == "add") return addSprite(a, "true");
                    if (name == "insert") {
                        if (a.size() < 2) throw Unsupported{"insert() needs two arguments"};
                        std::vector<HxPtr> rest(a.begin() + 1, a.end());
                        return addSprite(rest, "false");
                    }
                    if (name == "remove") return removeSprite(a, false);
                    if (name == "endSong") return callVal("endSong()");
                    if (name == "startVideo") return callVal("startVideo(" + arg(a, 0, "''") + ")");
                    return callVal("callMethod(" + luaStr(name) + ", " + callArgs(a) + ")");
                }
                return callVal("callMethod(" + pathExpr(base) + " .. " + luaStr("." + name) + ", " + callArgs(a) + ")");
            }
            case VK::Stage: {
                if (name == "getNamedProp" || name == "getSprite") {
                    if (a.empty()) throw Unsupported{"getNamedProp() needs a name"};
                    Val v = ev(a[0]);
                    if (a[0]->k == HxK::Str) return objLit(a[0]->s);
                    return objExpr(rd(v));
                }
                if (name == "getBoyfriend") return objLit("boyfriend");
                if (name == "getDad") return objLit("dad");
                if (name == "getGirlfriend") return objLit("gf");
                if (name == "add" || name == "addProp" || name == "addBopper") return addSprite(a, "false");
                if (name == "remove") return removeSprite(a, false);
                if (name == "refresh") return noneVal();
                throw Unsupported{"stage." + name + "() has no Lua equivalent"};
            }
            case VK::Cls:
                if (base.segs.empty()) return clsCall(base.cls, name, a, line);
                return callVal("callMethodFromClass(" + luaStr(base.cls) + ", " + pathExpr(base) + " .. " + luaStr("." + name) + ", " + callArgs(a) + ")");
            case VK::Ns: return nsCall(base, name, a, line);
            case VK::Event: return eventCall(base, name, a, line);
            case VK::Lua: return luaMethod(base, name, a, line);
        }
        throw Unsupported{name + "() has no Lua equivalent"};
    }

    Val clsCall(const std::string& cls, const std::string& name, const std::vector<HxPtr>& a, int) {
        if (cls == "backend.Paths" || cls == "funkin.Paths") return pathsCall(name, a);
        std::string last = lastSegment(cls);
        if (last == "HapticUtil") return noneVal();
        if (last == "FunkinMemory") {
            if (name == "cacheSound" || name == "cacheMusic") return callVal("precacheSound(" + arg(a, 0, "''") + ")");
            if (name == "cacheTexture" || name == "permanentCacheTexture") return callVal("precacheImage(" + arg(a, 0, "''") + ")");
            return noneVal();
        }
        return callVal("callMethodFromClass(" + luaStr(cls) + ", " + luaStr(name) + ", " + callArgs(a) + ")");
    }

    Val pathsCall(const std::string& name, const std::vector<HxPtr>& a) {
        static const std::set<std::string> keyed = {"image", "sound", "music", "getSparrowAtlas", "getPackerAtlas", "font", "inst", "voices",
            "getAtlas", "animateAtlas", "file", "json", "txt", "xml", "videos", "video", "frag", "vert", "shaderFragment"};
        if (name == "soundRandom") {
            return strVal(arg(a, 0, "''") + " .. getRandomInt(" + arg(a, 1, "0") + ", " + arg(a, 2, "1") + ")", P_CAT);
        }
        if (keyed.count(name)) {
            if (a.empty()) throw Unsupported{"Paths." + name + "() needs a key"};
            Val v = ev(a[0]);
            v.str = true;
            return v;
        }
        throw Unsupported{"Paths." + name + "() has no Lua equivalent"};
    }

    Val nsCall(const Val& base, const std::string& name, const std::vector<HxPtr>& a, int line) {
        const std::string& n = base.cls;
        if (n == "Paths") return pathsCall(name, a);
        if (n == "Math") {
            static const std::set<std::string> direct = {"floor", "ceil", "abs", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "exp", "log",
                "max", "min"};
            if (direct.count(name)) return numVal("math." + name + "(" + argList(a) + ")");
            if (name == "pow") return numVal(arg(a, 0, "0") + " ^ " + arg(a, 1, "1"), P_POW);
            if (name == "round" || name == "fround") return numVal("math.floor(" + (a.empty() ? "0" : L(a[0], P_ADD)) + " + 0.5)");
            if (name == "ffloor") return numVal("math.floor(" + argList(a) + ")");
            if (name == "fceil") return numVal("math.ceil(" + argList(a) + ")");
            if (name == "random") return numVal("math.random()");
            if (name == "atan2") return numVal("math.atan2(" + argList(a) + ")");
            if (name == "isNaN") {
                std::string x = arg(a, 0, "0");
                return lua(x + " ~= " + x, P_CMP);
            }
            if (name == "isFinite") {
                std::string x = arg(a, 0, "0");
                return lua("(" + x + " == " + x + " and " + x + " ~= math.huge and " + x + " ~= -math.huge)");
            }
        }
        if (n == "Std") {
            if (name == "int") return numVal(useHelper("hx_int") + "(" + argList(a) + ")");
            if (name == "string") return strVal("tostring(" + argList(a) + ")");
            if (name == "parseFloat" || name == "parseInt") return numVal("tonumber(" + argList(a) + ")");
            if (name == "random") return numVal("getRandomInt(0, " + (a.empty() ? "0" : L(a[0], P_ADD)) + " - 1)");
            if (name == "downcast" || name == "instance") return ev(a.at(0));
        }
        if (n == "StringTools") {
            static const std::map<std::string, std::string> helpers = {{"startsWith", "hx_startsWith"}, {"endsWith", "hx_endsWith"},
                {"replace", "hx_replace"}, {"trim", "hx_trim"}, {"ltrim", "hx_ltrim"}, {"rtrim", "hx_rtrim"}, {"lpad", "hx_lpad"},
                {"rpad", "hx_rpad"}, {"contains", "hx_contains"}};
            auto h = helpers.find(name);
            if (h != helpers.end()) {
                Val v = lua(useHelper(h->second) + "(" + argList(a) + ")");
                v.str = name != "startsWith" && name != "endsWith" && name != "contains";
                return v;
            }
            if (name == "hex") return strVal("string.format('%X', " + arg(a, 0, "0") + ")");
            if (name == "isSpace") return lua("string.match(string.sub(" + arg(a, 0, "''") + ", " + arg(a, 1, "0") + " + 1, " + arg(a, 1, "0") + " + 1), '%s') ~= nil", P_CMP);
        }
        if (n == "FlxMath") {
            if (name == "lerp") {
                std::string x = arg(a, 0, "0");
                return numVal(L(a.at(0), P_ADD) + " + (" + arg(a, 1, "0") + " - " + L(a.at(0), P_ADD + 1) + ") * " + (a.size() > 2 ? L(a[2], P_MUL + 1) : "1"), P_ADD);
            }
            if (name == "bound") return numVal(useHelper("hx_bound") + "(" + argList(a) + ")");
            if (name == "roundDecimal") return numVal(useHelper("hx_roundDecimal") + "(" + argList(a) + ")");
            if (name == "remapToRange") return numVal(useHelper("hx_remap") + "(" + argList(a) + ")");
            if (name == "maxInt") return numVal("math.max(" + argList(a) + ")");
            if (name == "minInt") return numVal("math.min(" + argList(a) + ")");
            if (name == "isEven") return lua(L(a.at(0), P_MUL) + " % 2 == 0", P_CMP);
            if (name == "isOdd") return lua(L(a.at(0), P_MUL) + " % 2 ~= 0", P_CMP);
            if (name == "fastSin") return numVal("math.sin(" + argList(a) + ")");
            if (name == "fastCos") return numVal("math.cos(" + argList(a) + ")");
            if (name == "signOf") return numVal("(" + arg(a, 0, "0") + " < 0 and -1 or 1)");
        }
        if (n == "random") {
            if (name == "int") return numVal("getRandomInt(" + arg(a, 0, "0") + ", " + arg(a, 1, "2147483647") + ")");
            if (name == "float") return numVal("getRandomFloat(" + arg(a, 0, "0") + ", " + arg(a, 1, "1") + ")");
            if (name == "bool") return lua("getRandomBool(" + arg(a, 0, "50") + ")");
            if (name == "sign") return numVal("(getRandomBool(" + arg(a, 0, "50") + ") and 1 or -1)");
            if (name == "getObject") {
                std::string arr = a.empty() ? "{}" : prefix(ev(a[0]));
                Val v = lua(arr + "[getRandomInt(1, #" + arr + ")]");
                Val src = a.empty() ? Val() : ev(a[0]);
                v.spr = src.sprArr;
                return v;
            }
        }
        if (n == "FlxColor") {
            if (name == "fromRGB") return numVal(useHelper("hx_rgb") + "(" + argList(a) + ")");
            if (name == "fromString") {
                if (!a.empty() && a[0]->k == HxK::Str) {
                    Val v = stringVal(a[0]);
                    if (!v.hex.empty()) {
                        Val c = numVal("getColorFromHex(" + luaStr(v.hex) + ")");
                        c.hex = v.hex;
                        return c;
                    }
                }
                return numVal("getColorFromHex(" + arg(a, 0, "''") + ")");
            }
            if (name == "fromInt") return ev(a.at(0));
        }
        if (n == "FunkinSprite") return creation(name, a);
        if (n == "FlxTween") return tween(name, a, line);
        if (n == "FlxTimer") {
            if (name == "wait" || name == "loop") {
                std::string tag = storedTag("hxTimer");
                startTimer(tag, a.size() > 0 ? a[0] : nullptr, a.size() > 1 ? a[1] : nullptr, name == "loop" && a.size() > 2 ? a[2] : nullptr, line);
                Val v = lua(tag);
                v.tagKind = "timer";
                return v;
            }
        }
        if (n == "sound") {
            if (name == "play") return playSound(a, a.size() > 1 ? a[1] : nullptr, a.size() > 2 ? a[2] : nullptr);
            if (name == "playMusic") return callVal("playMusic(" + arg(a, 0, "''") + ", " + arg(a, 1, "1") + ", " + arg(a, 2, "true") + ")");
            Val v = clsVal("flixel.FlxG");
            v.segs.push_back({true, ".sound"});
            return callVal("callMethodFromClass('flixel.FlxG', " + luaStr("sound." + name) + ", " + callArgs(a) + ")");
        }
        if (n == "FunkinSound" || n == "FlxSound") {
            if (name == "playOnce") return playSound(a, a.size() > 1 ? a[1] : nullptr, nullptr);
            if (name == "load" && a.size() > 4 && a[4]->k == HxK::True) return playSound(a, a.size() > 1 ? a[1] : nullptr, a.size() > 2 ? a[2] : nullptr);
            if (name == "playMusic") return callVal("playMusic(" + arg(a, 0, "''") + ", 1, true)");
        }
        if (n == "VideoCutscene" && name == "play") return callVal("startVideo(" + arg(a, 0, "''") + ")");
        if (n == "PlayState" || n == "conductor" || n == "Conductor" || n == "FlxG" || n == "prefs") {
            std::string cls = n == "PlayState" ? "states.PlayState" : n == "FlxG" ? "flixel.FlxG" : n == "prefs" ? "backend.ClientPrefs" : "backend.Conductor";
            return callVal("callMethodFromClass(" + luaStr(cls) + ", " + luaStr(name) + ", " + callArgs(a) + ")");
        }
        if (n == "cnestage" && name == "getSprite" && !a.empty()) {
            if (a[0]->k == HxK::Str) return objLit(a[0]->s);
            return objExpr(L(a[0]));
        }
        if (n == "timerParam") {
            size_t bar = base.path.find('|');
            std::string tag = base.path.substr(0, bar);
            if (name == "cancel") return callVal("cancelTimer(" + tag + ")");
        }
        if (n == "Reflect") {
            if (name == "field" && a.size() == 2) {
                Val obj = ev(a[0]);
                if (a[1]->k == HxK::Str) return field(obj, a[1]->s, line);
            }
            if (name == "setField" && a.size() == 3 && a[1]->k == HxK::Str) {
                Out o;
                HxPtr target = std::make_shared<HxNode>();
                target->k = HxK::Field;
                target->a = a[0];
                target->s = a[1]->s;
                target->line = line;
                assignTo(target, "=", ev(a[2]), o);
                for (size_t i = 0; i + 1 < o.lines.size(); i++) emitPre(o.lines[i]);
                return callVal(o.lines.empty() ? "" : o.lines.back());
            }
        }
        throw Unsupported{n + "." + name + "() has no Lua equivalent"};
    }

    Val playSound(const std::vector<HxPtr>& a, const HxPtr& volume, const HxPtr& loop) {
        std::string key = arg(a, 0, "''");
        std::string vol = volume ? L(volume) : "1";
        std::string out = "playSound(" + key + ", " + vol;
        std::string tag;
        if (!wantTag_.empty()) {
            tag = storedTag("hxSound");
            out += ", " + tag;
        } else if (loop) {
            out += ", nil";
        }
        if (loop) out += ", " + L(loop);
        out += ")";
        if (!tag.empty()) {
            emitPre(out);
            Val v = lua(tag);
            v.tagKind = "sound";
            return v;
        }
        return callVal(out);
    }

    std::string tweenOptions(const HxPtr& options, std::string& onComplete, int line) {
        if (!options) return "";
        if (options->k != HxK::ObjLit) throw Unsupported{"tween options must be written inline to convert"};
        std::string out;
        for (size_t i = 0; i < options->keys.size(); i++) {
            const std::string& key = options->keys[i];
            const HxPtr& value = options->list[i];
            if (key == "onComplete") {
                if (value->k == HxK::Null) continue;
                onComplete = callbackFunction(value);
                continue;
            }
            if (key == "onUpdate" || key == "onStart") {
                issue(line, "the tween's " + key + " callback has no Lua equivalent and was dropped.");
                continue;
            }
            if (!out.empty()) out += ", ";
            out += key + " = " + L(value);
        }
        return out;
    }

    std::string callbackFunction(const HxPtr& value) {
        if (value->k == HxK::Func) {
            std::vector<HxParam> none;
            Sym param;
            if (!value->params.empty()) {
                param.kind = "tweenParam";
                param.lua = "nil";
            }
            return function(none, value->b, value->params.empty() ? std::map<std::string, Sym>() : std::map<std::string, Sym>{{value->params[0].name, param}});
        }
        Val f = ev(value);
        return "function() " + prefix(f) + "() end";
    }

    std::string tweenTarget(const Val& raw) {
        Val v = asObj(raw);
        if (v.k == VK::Obj) return pathExpr(v);
        if (v.k == VK::Game && v.segs.empty()) return "";
        throw Unsupported{"only sprites, cameras and characters can be tweened from Lua"};
    }

    Val tween(const std::string& name, const std::vector<HxPtr>& a, int line) {
        if (name == "tween") {
            if (a.size() < 3) throw Unsupported{"FlxTween.tween() needs a target, values and a duration"};
            Val targetVal = ev(a[0]);
            std::string target = tweenTarget(targetVal);
            if (a[1]->k != HxK::ObjLit) throw Unsupported{"tween values must be written inline to convert"};
            std::string values;
            std::vector<std::string> sets;
            for (size_t i = 0; i < a[1]->keys.size(); i++) {
                std::string key = a[1]->keys[i];
                std::string value = L(a[1]->list[i]);
                if (target.empty()) {
                    if (key == "currentCameraZoom" || key == "defaultCameraZoom" || key == "defaultCamZoom") {
                        sets.push_back("setProperty('defaultCamZoom', " + value + ")");
                        key = "zoom";
                    } else {
                        throw Unsupported{"tweening PlayState." + key + " has no Lua equivalent"};
                    }
                }
                if (!values.empty()) values += ", ";
                values += (isIdent(key) ? key : "[" + luaStr(key) + "]") + " = " + value;
            }
            if (target.empty()) target = "'camGame'";
            std::string done;
            std::string options = tweenOptions(a.size() > 3 ? a[3] : nullptr, done, line);
            std::string tag = storedTag("hxTween");
            if (!done.empty()) {
                tweens_ = true;
                emitPre("hx_tweens[" + tag + "] = " + done);
                if (!options.empty()) options += ", ";
                options += "onComplete = 'hxTweenDone_" + uniq_ + "'";
            }
            for (const std::string& s : sets) emitPre(s);
            emitPre("startTween(" + tag + ", " + target + ", {" + values + "}, " + L(a[2]) + (options.empty() ? "" : ", {" + options + "}") + ")");
            Val v = lua(tag);
            v.tagKind = "tween";
            return v;
        }
        if (name == "color") {
            if (a.size() < 4) throw Unsupported{"FlxTween.color() needs a target, duration and two colors"};
            Val targetObj = asObj(ev(a[0]));
            std::string target = tweenTarget(targetObj);
            std::string done;
            std::string ease = "'linear'";
            if (a.size() > 4) {
                if (a[4]->k != HxK::ObjLit) throw Unsupported{"tween options must be written inline to convert"};
                for (size_t i = 0; i < a[4]->keys.size(); i++) {
                    if (a[4]->keys[i] == "ease") ease = L(a[4]->list[i]);
                    if (a[4]->keys[i] == "onComplete" && a[4]->list[i]->k != HxK::Null) done = callbackFunction(a[4]->list[i]);
                }
            }
            std::string tag = storedTag("hxTween");
            if (!done.empty()) {
                tweens_ = true;
                tweenEvents_ = true;
                emitPre("hx_tweens[" + tag + "] = " + done);
            }
            emitPre("setProperty(" + joinPath(targetObj, ".color") + ", " + colorNumber(a[2]) + ")");
            emitPre("doTweenColor(" + tag + ", " + target + ", " + colorHex(a[3]) + ", " + L(a[1]) + ", " + ease + ")");
            Val v = lua(tag);
            v.tagKind = "tween";
            return v;
        }
        if (name == "angle") {
            if (a.size() < 4) throw Unsupported{"FlxTween.angle() needs a target, two angles and a duration"};
            Val targetObj = asObj(ev(a[0]));
            std::string target = tweenTarget(targetObj);
            std::string done;
            std::string options = tweenOptions(a.size() > 4 ? a[4] : nullptr, done, line);
            std::string tag = storedTag("hxTween");
            if (!done.empty()) {
                tweens_ = true;
                emitPre("hx_tweens[" + tag + "] = " + done);
                if (!options.empty()) options += ", ";
                options += "onComplete = 'hxTweenDone_" + uniq_ + "'";
            }
            emitPre("setProperty(" + joinPath(targetObj, ".angle") + ", " + L(a[1]) + ")");
            emitPre("startTween(" + tag + ", " + target + ", {angle = " + L(a[2]) + "}, " + L(a[3]) + (options.empty() ? "" : ", {" + options + "}") + ")");
            Val v = lua(tag);
            v.tagKind = "tween";
            return v;
        }
        throw Unsupported{"FlxTween." + name + "() has no Lua equivalent"};
    }

    void startTimer(const std::string& tag, const HxPtr& time, const HxPtr& fn, const HxPtr& loops, int) {
        if (fn && fn->k != HxK::Null) {
            timers_ = true;
            std::string body;
            if (fn->k == HxK::Func) {
                std::map<std::string, Sym> bind;
                std::string p = fn->params.empty() ? "tmr" : fn->params[0].name;
                std::string base = sanitize(p);
                std::string lt = base + "Tag";
                std::string ll = base + "Loops";
                std::string lf = base + "Left";
                if (!fn->params.empty()) {
                    Sym s;
                    if (fn->params[0].type == "Int") {
                        s.lua = "(" + ll + " - " + lf + ")";
                    } else {
                        s.kind = "timerParam";
                        s.extra = lt + "|" + ll + "|" + lf;
                    }
                    bind[fn->params[0].name] = s;
                }
                std::vector<HxParam> params(3);
                params[0].name = lt;
                params[1].name = ll;
                params[2].name = lf;
                body = function(params, fn->b, bind, true);
            } else {
                body = "function() " + prefix(ev(fn)) + "() end";
            }
            emitPre("hx_timers[" + tag + "] = " + body);
        }
        emitPre("runTimer(" + tag + ", " + (time ? L(time) : "1") + ", " + (loops ? L(loops) : "1") + ")");
    }

    Val objMethod(const Val& v, const std::string& name, const std::vector<HxPtr>& a, int line) {
        Val head = v;
        head.segs.resize(1);
        std::string tag = tagExpr(head);
        bool direct = v.segs.size() == 1;
        std::string cam = cameraName(v);
        if (direct && !cam.empty()) {
            if (name == "flash") return callVal("cameraFlash(" + luaStr(cam) + ", " + (a.empty() ? "'FFFFFF'" : colorHex(a[0])) + ", " + arg(a, 1, "1") + ", " + arg(a, 3, "false") + ")");
            if (name == "shake") return callVal("cameraShake(" + luaStr(cam) + ", " + arg(a, 0, "0.05") + ", " + arg(a, 1, "0.5") + ")");
            if (name == "fade") return callVal("cameraFade(" + luaStr(cam) + ", " + (a.empty() ? "'000000'" : colorHex(a[0])) + ", " + arg(a, 1, "1") + ", true)");
        }
        if (direct) {
            if (name == "loadGraphic") {
                emitPre("loadGraphic(" + tag + ", " + arg(a, 0, "''") + (a.size() > 2 ? ", " + L(a[2]) + ", " + arg(a, 3, "0") : "") + ")");
                return v;
            }
            if (name == "loadTexture") {
                // FunkinSprite.loadTexture(path) is a still image; loadSparrow(path) is an animated atlas.
                emitPre("loadGraphic(" + tag + ", " + arg(a, 0, "''") + ")");
                return v;
            }
            if (name == "loadSparrow") {
                emitPre("loadFrames(" + tag + ", " + arg(a, 0, "''") + ")");
                return v;
            }
            if (name == "makeGraphic" || name == "makeSolidColor") {
                emitPre("makeGraphic(" + tag + ", " + arg(a, 0, "1") + ", " + arg(a, 1, "1") + ", " + (a.size() > 2 ? colorHex(a[2]) : "'FFFFFF'") + ")");
                return v;
            }
            if (name == "screenCenter") {
                emitPre("screenCenter(" + tag + (a.empty() ? "" : ", " + L(a[0])) + ")");
                return v;
            }
            if (name == "setGraphicSize") return callVal("setGraphicSize(" + tag + ", " + arg(a, 0, "0") + ", " + arg(a, 1, "0") + ", false)");
            if (name == "updateHitbox") return callVal("updateHitbox(" + tag + ")");
            if (name == "setPosition") {
                if (a.size() > 1) emitPre("setProperty(" + joinPath(head, ".x") + ", " + L(a[0]) + ")");
                return callVal("setProperty(" + (a.size() > 1 ? joinPath(head, ".y") + ", " + L(a[1]) : joinPath(head, ".x") + ", " + arg(a, 0, "0")) + ")");
            }
            if (name == "playAnimation" || name == "playAnim") return callVal("playAnim(" + tag + ", " + arg(a, 0, "''") + ", " + arg(a, 1, "false") + ")");
            if (name == "dance") {
                std::string t = literalTag(v);
                if (t == "boyfriend" || t == "dad" || t == "gf" || t.empty()) return callVal("characterDance(" + tag + ")");
            }
            if (name == "getCurrentAnimation") return strVal("getProperty(" + joinPath(head, ".animation.curAnim.name") + ")");
            if (name == "isAnimationFinished") return lua("getProperty(" + joinPath(head, ".animation.curAnim.finished") + ")");
            if (name == "destroy") return callVal(std::string(v.text ? "removeLuaText(" : "removeLuaSprite(") + tag + ", true)");
            if (name == "addOffset") return callVal("addOffset(" + tag + ", " + argList(a) + ")");
            if (name == "loadFrames") return callVal("loadFrames(" + tag + ", " + arg(a, 0, "''") + ")");
            if (name == "setFormat") {
                if (a.size() > 0 && a[0]->k != HxK::Null) emitPre("setTextFont(" + tag + ", " + L(a[0]) + ")");
                if (a.size() > 2) emitPre("setTextColor(" + tag + ", " + colorHex(a[2]) + ")");
                if (a.size() > 3) emitPre("setTextAlignment(" + tag + ", " + L(a[3]) + ")");
                if (a.size() > 5) emitPre("setTextBorder(" + tag + ", 1, " + colorHex(a[5]) + ")");
                return callVal("setTextSize(" + tag + ", " + arg(a, 1, "8") + ")");
            }
        }
        if (subIs(v, ".animation")) {
            if (name == "play") return callVal("playAnim(" + tag + ", " + arg(a, 0, "''") + ", " + arg(a, 1, "false") + (a.size() > 2 ? ", " + L(a[2]) + ", " + arg(a, 3, "0") : "") + ")");
            if (name == "addByPrefix") return callVal("addAnimationByPrefix(" + tag + ", " + arg(a, 0, "''") + ", " + arg(a, 1, "''") + ", " + arg(a, 2, "24") + ", " + arg(a, 3, "true") + ")");
            if (name == "addByIndices") {
                std::string indices = "{}";
                if (a.size() > 2) {
                    if (a[2]->k == HxK::ArrayLit) {
                        std::string joined;
                        bool literal = true;
                        for (const HxPtr& i : a[2]->list) {
                            if (i->k != HxK::Int) literal = false;
                            else joined += (joined.empty() ? "" : ",") + numText(i->num);
                        }
                        indices = literal ? luaStr(joined) : L(a[2]);
                    } else {
                        indices = L(a[2]);
                    }
                }
                return callVal("addAnimationByIndices(" + tag + ", " + arg(a, 0, "''") + ", " + arg(a, 1, "''") + ", " + indices + ", " + arg(a, 4, "24") + ", " + arg(a, 5, "true") + ")");
            }
            if (name == "add") return callVal("addAnimation(" + tag + ", " + arg(a, 0, "''") + ", " + arg(a, 1, "{}") + ", " + arg(a, 2, "24") + ", " + arg(a, 3, "true") + ")");
        }
        if (name == "set" && a.size() <= 2) {
            if (subIs(v, ".scale")) return callVal("scaleObject(" + tag + ", " + arg(a, 0, "1") + ", " + (a.size() > 1 ? L(a[1]) : arg(a, 0, "1")) + ", false)");
            if (subIs(v, ".scrollFactor")) return callVal("setScrollFactor(" + tag + ", " + arg(a, 0, "1") + ", " + (a.size() > 1 ? L(a[1]) : arg(a, 0, "1")) + ")");
            std::string x = arg(a, 0, "0");
            std::string y = a.size() > 1 ? L(a[1]) : x;
            emitPre("setProperty(" + joinPath(v, ".x") + ", " + x + ")");
            return callVal("setProperty(" + joinPath(v, ".y") + ", " + y + ")");
        }
        (void)line;
        return callVal("callMethod(" + joinPath(v, "." + name) + ", " + callArgs(a) + ")");
    }

    Val luaMethod(const Val& base, const std::string& name, const std::vector<HxPtr>& a, int line) {
        std::string b = prefix(base);
        if (base.tagKind == "tween") {
            if (name == "cancel") return callVal("cancelTween(" + b + ")");
            throw Unsupported{"tween." + name + "() has no Lua equivalent"};
        }
        if (base.tagKind == "timer") {
            if (name == "start") {
                startTimer(b, a.size() > 0 ? a[0] : nullptr, a.size() > 1 ? a[1] : nullptr, a.size() > 2 ? a[2] : nullptr, line);
                return noneVal();
            }
            if (name == "cancel") return callVal("cancelTimer(" + b + ")");
            throw Unsupported{"timer." + name + "() has no Lua equivalent"};
        }
        if (base.tagKind == "sound") {
            if (name == "stop") return callVal("stopSound(" + b + ")");
            if (name == "pause") return callVal("pauseSound(" + b + ")");
            if (name == "resume" || name == "play") return callVal("resumeSound(" + b + ")");
            if (name == "fadeOut") return callVal("soundFadeOut(" + b + ", " + arg(a, 0, "1") + ", " + arg(a, 1, "0") + ")");
            if (name == "fadeIn") return callVal("soundFadeIn(" + b + ", " + arg(a, 0, "1") + ", " + arg(a, 1, "0") + ", " + arg(a, 2, "1") + ")");
            throw Unsupported{"sound." + name + "() has no Lua equivalent"};
        }
        static const std::map<std::string, std::string> extensions = {{"startsWith", "hx_startsWith"}, {"endsWith", "hx_endsWith"},
            {"replace", "hx_replace"}, {"trim", "hx_trim"}, {"ltrim", "hx_ltrim"}, {"rtrim", "hx_rtrim"}, {"lpad", "hx_lpad"}, {"rpad", "hx_rpad"}};
        auto ext = extensions.find(name);
        if (ext != extensions.end() && (base.str || name != "replace" || a.size() == 2)) {
            Val v = lua(useHelper(ext->second) + "(" + b + (a.empty() ? "" : ", " + argList(a)) + ")");
            v.str = name != "startsWith" && name != "endsWith";
            return v;
        }
        if (name == "push") {
            Val v = a.empty() ? lua("nil") : ev(a[0]);
            return callVal("table.insert(" + b + ", " + rd(v) + ")");
        }
        if (name == "pop") return lua("table.remove(" + b + ")");
        if (name == "shift") return lua("table.remove(" + b + ", 1)");
        if (name == "unshift") return callVal("table.insert(" + b + ", 1, " + arg(a, 0, "nil") + ")");
        if (name == "insert") return callVal("table.insert(" + b + ", " + (a.empty() ? "1" : L(a[0], P_ADD)) + " + 1, " + arg(a, 1, "nil") + ")");
        if (name == "join") return strVal("table.concat(" + b + ", " + arg(a, 0, "','") + ")");
        if (name == "copy") {
            Val v = lua(useHelper("hx_copy") + "(" + b + ")");
            v.arr = base.arr;
            v.map = base.map;
            v.sprArr = base.sprArr;
            return v;
        }
        if (name == "toString") return strVal("tostring(" + b + ")");
        if (name == "toUpperCase") return strVal("string.upper(" + b + ")");
        if (name == "toLowerCase") return strVal("string.lower(" + b + ")");
        if (name == "charAt") {
            std::string i = a.empty() ? "0" : L(a[0], P_ADD);
            return strVal("string.sub(" + b + ", " + i + " + 1, " + i + " + 1)");
        }
        if (name == "charCodeAt") return numVal("string.byte(" + b + ", " + (a.empty() ? "0" : L(a[0], P_ADD)) + " + 1)");
        if (name == "substr") return strVal(useHelper("hx_substr") + "(" + b + ", " + argList(a) + ")");
        if (name == "substring") return strVal(useHelper("hx_substring") + "(" + b + ", " + argList(a) + ")");
        if (name == "split") {
            Val v = lua(useHelper("hx_split") + "(" + b + ", " + arg(a, 0, "''") + ")");
            v.arr = true;
            return v;
        }
        if (base.map) {
            if (name == "get") return lua(b + "[" + arg(a, 0, "nil") + "]");
            if (name == "set") return callVal(useHelper("hx_set") + "(" + b + ", " + argList(a) + ")");
            if (name == "exists") return lua(b + "[" + arg(a, 0, "nil") + "] ~= nil", P_CMP);
            if (name == "remove") return callVal(useHelper("hx_mapRemove") + "(" + b + ", " + arg(a, 0, "nil") + ")");
            if (name == "keys") {
                Val v = lua(useHelper("hx_keys") + "(" + b + ")");
                v.arr = true;
                return v;
            }
            if (name == "clear") return callVal(useHelper("hx_clear") + "(" + b + ")");
        }
        if (name == "get") return lua(b + "[" + arg(a, 0, "nil") + "]");
        if (name == "set") {
            helpers_.insert("hx_set");
            return callVal("hx_set(" + b + ", " + argList(a) + ")");
        }
        if (name == "exists") return lua(b + "[" + arg(a, 0, "nil") + "] ~= nil", P_CMP);
        if (name == "indexOf") return numVal(useHelper("hx_indexOf") + "(" + b + ", " + arg(a, 0, "nil") + ")");
        if (name == "lastIndexOf") return numVal(useHelper("hx_lastIndexOf") + "(" + b + ", " + arg(a, 0, "nil") + ")");
        if (name == "contains") return lua(useHelper("hx_contains") + "(" + b + ", " + arg(a, 0, "nil") + ")");
        if (name == "remove") return lua(useHelper("hx_remove") + "(" + b + ", " + arg(a, 0, "nil") + ")");
        if (name == "concat") {
            Val v = lua(useHelper("hx_concat") + "(" + b + ", " + arg(a, 0, "{}") + ")");
            v.arr = true;
            return v;
        }
        if (name == "slice") {
            Val v = lua(useHelper("hx_slice") + "(" + b + ", " + argList(a) + ")");
            v.arr = true;
            return v;
        }
        if (name == "splice") {
            Val v = lua(useHelper("hx_splice") + "(" + b + ", " + argList(a) + ")");
            v.arr = true;
            return v;
        }
        if (name == "reverse") return callVal(useHelper("hx_reverse") + "(" + b + ")");
        if (name == "filter" || name == "map") {
            Val v = lua(useHelper("hx_" + name) + "(" + b + ", " + arg(a, 0, "nil") + ")");
            v.arr = true;
            return v;
        }
        if (name == "sort") return callVal("table.sort(" + b + ", function(hx_a, hx_b) return " + (a.empty() ? "hx_a < hx_b" : prefix(ev(a[0])) + "(hx_a, hx_b) < 0") + " end)");
        if (name == "resize") return noneVal();
        if (isIdent(name) && !kLuaKeywords.count(name)) return callVal(b + "." + name + "(" + argList(a) + ")");
        throw Unsupported{name + "() has no Lua equivalent"};
    }

    Val eventCall(const Val& base, const std::string& name, const std::vector<HxPtr>& a, int line) {
        if (name == "cancel" || name == "cancelEvent" || name == "preventDefault") {
            if (!cancelable_) {
                issue(line, "this Psych callback can't be cancelled; event." + name + "() was dropped.");
                return noneVal();
            }
            cancelUsed_ = true;
            if (eventKind_ == "note") issue(line, "Psych can't cancel a note hit; Function_Stop is returned, but the hit still counts.");
            return callVal("hx_stop = true");
        }
        if (name == "cancelAnim" || name == "preventAnim" || name == "cancelStrumGlow" || name == "preventStrumGlow" || name == "preventDeletion" ||
            name == "cancelDeletion") {
            issue(line, "event." + name + "() has no Psych equivalent and was dropped.");
            return noneVal();
        }
        if ((name == "getString" || name == "getFloat" || name == "getInt" || name == "getBool" || name == "getDynamic") && !a.empty() &&
            a[0]->k == HxK::Str && (base.path == "eventData" || base.path.empty())) {
            std::string raw = valueRead(a[0]->s, 0);
            if (raw == "nil") return lua("nil");
            std::string plain = raw;
            size_t valuePos = raw.find("value");
            if (valuePos != std::string::npos) plain = raw.substr(valuePos, 6);
            if (name == "getFloat" || name == "getInt") return numVal("tonumber(" + plain + ")");
            if (name == "getBool") return lua(plain + " == 'true'", P_CMP);
            return strVal(plain);
        }
        Val v = base;
        v.path += (v.path.empty() ? "" : ".") + name + "()";
        auto f = eventMap_.find(v.path);
        if (f != eventMap_.end()) return lua(f->second, f->second.find(' ') == std::string::npos ? P_ATOM : P_OR);
        throw Unsupported{"event." + v.path + " has no Psych equivalent"};
    }

    Val assignExpr(const HxPtr& n) {
        Out o;
        assignTo(n->a, n->s, ev(n->b), o);
        std::string target;
        try {
            target = L(n->a);
        } catch (const Unsupported&) {
            target = "nil";
        }
        std::string body;
        for (const std::string& l : o.lines) body += l + " ";
        return lua("(function() " + body + "return " + target + " end)()");
    }

    std::string compound(const std::string& op, const std::string& current, const Val& value) {
        std::string bin = op.substr(0, op.size() - 1);
        if (bin == "+" && value.str) return current + " .. " + rd(value, P_CAT);
        static const std::map<std::string, std::string> bits = {{"&", "bit.band"}, {"|", "bit.bor"}, {"^", "bit.bxor"}, {"<<", "bit.lshift"},
            {">>", "bit.arshift"}, {">>>", "bit.rshift"}};
        auto b = bits.find(bin);
        if (b != bits.end()) return b->second + "(" + current + ", " + rd(value) + ")";
        if (bin == "??") return useHelper("hx_coalesce") + "(" + current + ", " + rd(value) + ")";
        int prec = bin == "+" || bin == "-" ? P_ADD : P_MUL;
        return current + " " + bin + " " + rd(value, prec + 1);
    }

    void assignTo(const HxPtr& target, const std::string& op, const Val& value, Out& out) {
        HxPtr t = target;
        while (t->k == HxK::Paren) t = t->a;
        if (t->k == HxK::Ident || (t->k == HxK::Field && t->a->k == HxK::This && (fields_.count(t->s) || !findSym(t->s)))) {
            const std::string& name = t->s;
            Sym* s = t->k == HxK::Ident ? findSym(name) : nullptr;
            if (!s) {
                auto f = fields_.find(name);
                if (f != fields_.end()) s = &f->second;
            }
            if (!s) {
                Val g = t->k == HxK::Ident ? global(name, t->line) : global(name, t->line);
                assignVal(g, op, value, out, t->line);
                return;
            }
            if (s->kind == "event" || s->kind == "self") throw Unsupported{"this can't be reassigned in Lua"};
            if (s->spr && s->lit) {
                if (value.k == VK::Obj && value.segs.size() == 1 && value.segs[0].lit && luaStr(value.segs[0].text) == s->lua) return;
                throw Unsupported{"a sprite field can't be pointed at another object in Lua"};
            }
            std::string current = s->lua;
            if (op == "=") {
                if (value.k == VK::Obj && value.segs.size() == 1) {
                    s->spr = true;
                    s->text = value.text;
                    out.line(s->lua + " = " + tagExpr(value));
                    return;
                }
                if (!value.tagKind.empty()) s->kind = value.tagKind;
                if (value.sprArr) s->sprArr = true;
                if (value.none) return;
                out.line(s->lua + " = " + rd(value));
                return;
            }
            out.line(s->lua + " = " + compound(op, current, value));
            return;
        }
        Val base;
        if (t->k == HxK::Field) {
            if (t->a->k == HxK::This) base = ident(t->s, t->line);
            else base = field(ev(t->a), t->s, t->line);
        } else if (t->k == HxK::Index) {
            base = index(t);
        } else {
            throw Unsupported{"this assignment target has no Lua equivalent"};
        }
        assignVal(base, op, value, out, t->line);
    }

    void assignVal(const Val& target, const std::string& op, const Val& value, Out& out, int line) {
        switch (target.k) {
            case VK::Obj: {
                if (target.segs.size() < 2) throw Unsupported{"a sprite can't be replaced in Lua"};
                const Seg& last = target.segs.back();
                Val owner = target;
                owner.segs.pop_back();
                std::string tag = tagOf(owner);
                if (last.lit && owner.segs.size() == 1) {
                    if (last.text == ".cameras" || last.text == ".camera") {
                        std::string cam = "game";
                        if (value.k == VK::Lua && value.lua.size() > 2 && value.lua.front() == '{') {
                            if (value.lua.find("camHUD") != std::string::npos) cam = "hud";
                            else if (value.lua.find("camOther") != std::string::npos) cam = "other";
                        } else if (value.k == VK::Obj) {
                            std::string c = cameraName(value);
                            if (!c.empty()) cam = c;
                        }
                        out.line("setObjectCamera(" + tag + ", " + luaStr(cam) + ")");
                        return;
                    }
                    if (last.text == ".frames") {
                        out.line("loadFrames(" + tag + ", " + rd(value) + ")");
                        return;
                    }
                    if (last.text == ".zIndex") {
                        std::string t = literalTag(owner);
                        if (!t.empty() && value.isNum) {
                            double z = std::strtod(value.lua.c_str(), nullptr);
                            zIndex_[t] = z >= opt_.characterLayer ? "true" : "false";
                        }
                        issue(line, "zIndex has no Lua equivalent; the sprite is added in front of or behind the characters instead.");
                        return;
                    }
                    if (last.text == ".shader") throw Unsupported{"shaders are set from Haxe code"};
                    if (last.text == ".color" && op == "=") {
                        std::string c;
                        if (!value.hex.empty()) c = "getColorFromHex(" + luaStr(value.hex) + ")";
                        else c = rd(value);
                        out.line("setProperty(" + pathExpr(target) + ", " + c + ")");
                        return;
                    }
                }
                std::string path = pathExpr(target);
                if (op == "=") out.line("setProperty(" + path + ", " + rd(value) + ")");
                else out.line("setProperty(" + path + ", " + compound(op, "getProperty(" + path + ")", value) + ")");
                return;
            }
            case VK::Game: {
                if (target.segs.empty()) throw Unsupported{"PlayState can't be replaced"};
                std::string path = pathExpr(target);
                if (op == "=") out.line("setProperty(" + path + ", " + rd(value) + ")");
                else out.line("setProperty(" + path + ", " + compound(op, "getProperty(" + path + ")", value) + ")");
                return;
            }
            case VK::Cls: {
                if (target.segs.empty()) throw Unsupported{"a class can't be replaced"};
                std::string path = pathExpr(target);
                std::string cls = luaStr(target.cls);
                if (op == "=") out.line("setPropertyFromClass(" + cls + ", " + path + ", " + rd(value) + ")");
                else out.line("setPropertyFromClass(" + cls + ", " + path + ", " + compound(op, "getPropertyFromClass(" + cls + ", " + path + ")", value) + ")");
                return;
            }
            case VK::Lua: {
                if (target.call) throw Unsupported{"this value can't be assigned in Lua"};
                if (target.tagKind == "sound") throw Unsupported{"sound fields are set with Lua functions"};
                std::string current = target.lua;
                if (current.compare(0, 15, "getSoundVolume(") == 0) {
                    out.line("setSoundVolume(" + current.substr(15, current.size() - 16) + ", " + rd(value) + ")");
                    return;
                }
                if (current.find('(') != std::string::npos && current.back() == ')') throw Unsupported{"this value can't be assigned in Lua"};
                if (op == "=") out.line(current + " = " + rd(value));
                else out.line(current + " = " + compound(op, current, value));
                return;
            }
            case VK::Event:
                issue(line, "changing event." + target.path + " has no Psych equivalent and was dropped.");
                return;
            default:
                throw Unsupported{"this assignment has no Lua equivalent"};
        }
    }

    std::string function(const std::vector<HxParam>& params, const HxPtr& body, const std::map<std::string, Sym>& bind, bool raw = false) {
        pushScope();
        std::string names;
        for (size_t i = 0; i < params.size(); i++) {
            if (i) names += ", ";
            if (raw) {
                names += params[i].name;
                continue;
            }
            Sym& s = declare(params[i].name);
            applyType(s, params[i].type, nullptr);
            if (typeIsSprite(params[i].type)) s.spr = true;
            names += s.lua;
        }
        for (const auto& b : bind) {
            Sym& s = scopes_.back()[b.first];
            s = b.second;
        }
        Out o;
        o.depth = 1;
        auto* savedPre = pre_;
        int savedLoop = loopDepth_;
        bool savedCallback = inCallback_;
        bool savedCreate = inCreate_;
        auto savedLabels = continueLabels_;
        auto savedUsed = continueUsed_;
        loopDepth_ = 0;
        inCallback_ = false;
        inCreate_ = false;
        continueLabels_.clear();
        continueUsed_.clear();
        for (const HxParam& p : params) {
            if (raw || !p.value) continue;
            Sym* s = findSym(p.name);
            std::vector<std::string> pre;
            pre_ = &pre;
            std::string value = L(p.value);
            for (const std::string& l : pre) o.line(l);
            o.line("if " + s->lua + " == nil then " + s->lua + " = " + value + " end");
        }
        if (body) {
            if (body->k == HxK::Block) block(body->list, o);
            else statement(body, o, true);
        }
        pre_ = savedPre;
        loopDepth_ = savedLoop;
        inCallback_ = savedCallback;
        inCreate_ = savedCreate;
        continueLabels_ = savedLabels;
        continueUsed_ = savedUsed;
        popScope();
        std::string out = "function(" + names + ")\n";
        for (const std::string& l : o.lines) out += l + "\n";
        return out + "end";
    }

    void block(const std::vector<HxPtr>& list, Out& out) {
        for (size_t i = 0; i < list.size(); i++) statement(list[i], out, i + 1 == list.size());
    }

    std::string haxeFor(const HxPtr& n, std::string& bring) {
        std::string text = src_.substr(n->start, n->end - n->start);
        std::set<std::string> brought;
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            char c = text[i];
            if (c == '"' || c == '\'') {
                char q = c;
                size_t j = i + 1;
                while (j < text.size() && text[j] != q) j += text[j] == '\\' ? 2 : 1;
                j = std::min(j + 1, text.size());
                out += text.substr(i, j - i);
                i = j;
                continue;
            }
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                size_t j = text.find('\n', i);
                if (j == std::string::npos) j = text.size();
                i = j;
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i;
                while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) j++;
                std::string word = text.substr(i, j - i);
                bool member = i > 0 && text[i - 1] == '.';
                bool thisMember = i >= 5 && text.compare(i - 5, 5, "this.") == 0;
                if (thisMember) {
                    out.erase(out.size() - 5);
                    member = false;
                }
                if (!member) {
                    Sym* s = findSym(word);
                    if (!s) {
                        auto f = fields_.find(word);
                        if (f != fields_.end()) s = &f->second;
                    }
                    if (s && s->kind != "event" && s->kind != "timerParam" && s->kind != "tweenParam") {
                        if (s->kind == "self") {
                            out += "game.getLuaObject(hx_self)";
                            brought.insert("hx_self");
                        } else if (s->spr && s->lit) {
                            out += "game.getLuaObject(\"" + s->lua.substr(1, s->lua.size() - 2) + "\")";
                        } else if (s->spr) {
                            out += "game.getLuaObject(" + s->lua + ")";
                            brought.insert(s->lua);
                        } else {
                            out += s->lua;
                            brought.insert(s->lua);
                        }
                        i = j;
                        continue;
                    }
                    if (methods_.count(word) && j < text.size() && text[j] == '(') issue(n->line, word + "() is a Lua function now and can't be called from the Haxe code kept in runHaxeCode.");
                    if (kind_ == "stage" && !codename_) {
                        if (word == "getBoyfriend" || word == "getDad" || word == "getGirlfriend") {
                            size_t k = j;
                            if (k + 1 < text.size() && text[k] == '(' && text[k + 1] == ')') {
                                out += word == "getBoyfriend" ? "game.boyfriend" : word == "getDad" ? "game.dad" : "game.gf";
                                i = k + 2;
                                continue;
                            }
                        }
                        if (word == "getNamedProp") {
                            out += "game.getLuaObject";
                            i = j;
                            continue;
                        }
                    }
                }
                out += word;
                i = j;
                continue;
            }
            out += c;
            i++;
        }
        static const std::vector<std::pair<std::string, std::string>> swaps = {{"PlayState.instance.currentStage.getBoyfriend()", "game.boyfriend"},
            {"PlayState.instance.currentStage.getDad()", "game.dad"}, {"PlayState.instance.currentStage.getGirlfriend()", "game.gf"},
            {"PlayState.instance.currentStage.getNamedProp(", "game.getLuaObject("}, {"PlayState.instance", "game"}, {".playAnimation(", ".playAnim("},
            {"Conductor.instance.", "Conductor."}, {"FunkinSprite", "FlxSprite"}, {"currentCameraZoom", "defaultCamZoom"}};
        for (const auto& s : swaps) {
            size_t pos = 0;
            while ((pos = out.find(s.first, pos)) != std::string::npos) {
                out.replace(pos, s.first.size(), s.second);
                pos += s.second.size();
            }
        }
        bring.clear();
        for (const std::string& b : brought) {
            if (!bring.empty()) bring += ", ";
            bring += b + " = " + b;
        }
        return out;
    }

    std::string longString(const std::string& raw) {
        std::string text = raw;
        if (!text.empty() && text.back() == ']') text += ' ';
        std::string eq;
        while (text.find("]" + eq + "]") != std::string::npos) eq += "=";
        return "[" + eq + "[" + text + "]" + eq + "]";
    }

    void fallback(const HxPtr& n, Out& out, const std::string& why) {
        fallbacks_++;
        std::string bring;
        std::string code = haxeFor(n, bring);
        std::string call = "runHaxeCode(" + longString(code) + (bring.empty() ? "" : ", {" + bring + "}") + ")";
        if (n->k == HxK::Var && n->names.size() == 1 && n->list[0]) {
            std::string bring2;
            std::string expr = haxeFor(n->list[0], bring2);
            Sym& s = declare(n->names[0]);
            out.line("local " + s.lua + " = runHaxeCode(" + longString(expr) + (bring2.empty() ? "" : ", {" + bring2 + "}") + ")");
        } else if (n->k == HxK::Var) {
            for (const std::string& name : n->names) {
                Sym& s = declare(name);
                out.line("local " + s.lua);
            }
            out.line(call);
        } else {
            out.line(call);
        }
        issue(n->line, "kept as Haxe inside runHaxeCode: " + why + ".");
    }

    void statement(const HxPtr& n, Out& out, bool last) {
        int savedLine = currentLine_;
        currentLine_ = n->line;
        struct Restore {
            int& ref;
            int value;
            ~Restore() { ref = value; }
        } restore{currentLine_, savedLine};
        Out tmp;
        tmp.depth = out.depth;
        std::vector<std::string> pre;
        auto* savedPre = pre_;
        pre_ = &pre;
        size_t scopeSize = scopes_.empty() ? 0 : scopes_.back().size();
        std::map<std::string, Sym> savedScope = scopes_.empty() ? std::map<std::string, Sym>() : scopes_.back();
        try {
            emit(n, tmp, last);
            pre_ = savedPre;
            for (const std::string& l : pre) out.line(l);
            out.append(tmp);
        } catch (const Unsupported& u) {
            pre_ = savedPre;
            if (!scopes_.empty() && scopes_.back().size() != scopeSize) scopes_.back() = savedScope;
            fallback(n, out, u.why);
        }
    }

    void declareVar(const std::string& name, const std::string& type, const HxPtr& init, Out& out) {
        if (!init) {
            Sym& s = declare(name);
            applyType(s, type, nullptr);
            if (typeIsSprite(type)) s.spr = true;
            out.line("local " + s.lua);
            return;
        }
        if (isCreation(init) || (init->k == HxK::New && lastSegment(init->s) == "FlxTimer") || isTagged(init)) {
            std::string lname = safe(name);
            if (kPsychApi.count(lname)) lname = "hx_" + lname;
            std::string tag;
            if (inCreate_ && loopDepth_ == 0 && !usedTags_.count(name) && isCreation(init)) {
                tag = luaStr(name);
                usedTags_.insert(name);
            } else {
                tag = isCreation(init) ? uidTag(name + "_") : "";
            }
            if (!tag.empty()) {
                emitPre("local " + lname + " = " + tag);
                wantTag_ = lname;
            } else {
                wantTag_ = "";
                counter_++;
                std::string prefix = init->k == HxK::New ? "hxTimer" : "hxTag";
                std::string base = prefix + "_" + uniq_ + "_" + std::to_string(counter_);
                std::string t = loopDepth_ > 0 || !inCreate_ ? uidTag(base + "_") : luaStr(base);
                emitPre("local " + lname + " = " + t);
                wantTag_ = lname;
            }
            Val v = ev(init);
            wantTag_.clear();
            Sym& s = declare(name);
            s.lua = lname;
            if (v.k == VK::Obj) {
                s.spr = true;
                s.text = v.text;
                std::string vt = tagExpr(v);
                if (vt != lname) out.line(lname + " = " + vt);
            } else {
                s.kind = v.tagKind;
                if (!v.none && rd(v) != lname) out.line(lname + " = " + rd(v));
            }
            return;
        }
        Val v = ev(init);
        Sym& s = declare(name);
        applyType(s, type, init);
        if (v.k == VK::Obj && v.segs.size() == 1) {
            s.spr = true;
            s.text = v.text;
            out.line("local " + s.lua + " = " + tagExpr(v));
            return;
        }
        if (v.k == VK::Lua && v.spr) s.spr = true;
        s.str = s.str || v.str;
        s.arr = s.arr || v.arr;
        s.map = s.map || v.map;
        s.sprArr = s.sprArr || v.sprArr;
        if (!v.tagKind.empty()) s.kind = v.tagKind;
        if (v.none) {
            out.line("local " + s.lua);
            return;
        }
        out.line("local " + s.lua + " = " + rd(v));
    }

    bool isTagged(const HxPtr& init) {
        if (init->k != HxK::Call || init->a->k != HxK::Field) return false;
        const HxPtr& callee = init->a;
        if (callee->a->k == HxK::Ident && callee->a->s == "FlxTween") return true;
        if (callee->a->k == HxK::Ident && (callee->a->s == "FunkinSound" || callee->a->s == "FlxTimer")) return true;
        if (callee->a->k == HxK::Field && callee->a->s == "sound" && callee->s == "play") return true;
        return false;
    }

    void emit(const HxPtr& n, Out& out, bool last) {
        switch (n->k) {
            case HxK::Block: {
                if (n->list.empty()) return;
                out.open("do");
                pushScope();
                block(n->list, out);
                popScope();
                out.close();
                return;
            }
            case HxK::Var: {
                if (n->s == "function") {
                    Sym& s = declare(n->names[0]);
                    out.line("local " + s.lua);
                    const HxPtr& f = n->list[0];
                    out.line(s.lua + " = " + function(f->params, f->b, {}));
                    return;
                }
                for (size_t i = 0; i < n->names.size(); i++) declareVar(n->names[i], n->keys[i], n->list[i], out);
                return;
            }
            case HxK::Assign: {
                const HxPtr& target = n->a;
                std::string name;
                if (target->k == HxK::Ident) name = target->s;
                if (target->k == HxK::Field && target->a->k == HxK::This) name = target->s;
                bool fieldTarget = !name.empty() && !findSym(name) && fields_.count(name);
                Sym* local = !name.empty() ? findSym(name) : nullptr;
                if (n->s == "=" && (fieldTarget || local) && (isCreation(n->b) || isTagged(n->b) || (n->b->k == HxK::New && lastSegment(n->b->s) == "FlxTimer"))) {
                    Sym& s = fieldTarget ? fields_[name] : *local;
                    if (s.spr && s.lit) {
                        wantTag_ = s.lua;
                    } else {
                        std::string tag;
                        if (isCreation(n->b)) tag = fieldTarget && !varModeFields_.count(name) ? luaStr(name) : (fieldTarget ? luaStr(name) : uidTag(name + "_"));
                        else {
                            counter_++;
                            std::string base = (n->b->k == HxK::New ? "hxTimer_" : "hxTag_") + uniq_ + "_" + std::to_string(counter_);
                            tag = loopDepth_ > 0 || !inCreate_ ? uidTag(base + "_") : luaStr(base);
                        }
                        emitPre(s.lua + " = " + tag);
                        wantTag_ = s.lua;
                    }
                    Val v = ev(n->b);
                    wantTag_.clear();
                    if (v.k == VK::Obj) {
                        s.spr = true;
                        s.text = s.text || v.text;
                    } else if (!v.tagKind.empty()) {
                        s.kind = v.tagKind;
                    }
                    return;
                }
                Val value = ev(n->b);
                assignTo(n->a, n->s, value, out);
                return;
            }
            case HxK::Unop:
            case HxK::Postfix:
                if (n->s == "++" || n->s == "--") {
                    assignTo(n->a, n->s == "++" ? "+=" : "-=", numVal("1"), out);
                    return;
                }
                break;
            case HxK::If: {
                std::string cond = L(n->a);
                out.open("if " + cond + " then");
                pushScope();
                body(n->b, out);
                popScope();
                HxPtr e = n->c;
                while (e && e->k == HxK::If) {
                    std::vector<std::string> pre;
                    auto* saved = pre_;
                    pre_ = &pre;
                    std::string c;
                    try {
                        c = L(e->a);
                    } catch (const Unsupported&) {
                        pre_ = saved;
                        break;
                    }
                    pre_ = saved;
                    if (!pre.empty()) break;
                    out.depth--;
                    out.open("elseif " + c + " then");
                    pushScope();
                    body(e->b, out);
                    popScope();
                    e = e->c;
                }
                if (e) {
                    out.depth--;
                    out.open("else");
                    pushScope();
                    body(e, out);
                    popScope();
                }
                out.close();
                return;
            }
            case HxK::While: {
                std::vector<std::string> pre;
                auto* saved = pre_;
                pre_ = &pre;
                std::string cond = L(n->a);
                pre_ = saved;
                if (!pre.empty()) throw Unsupported{"this loop condition needs setup code"};
                out.open("while " + cond + " do");
                loopBody(n->b, out, nullptr);
                out.close();
                return;
            }
            case HxK::DoWhile: {
                out.open("repeat");
                loopBody(n->b, out, nullptr);
                std::vector<std::string> pre;
                auto* saved = pre_;
                pre_ = &pre;
                std::string cond = L(n->a, P_UN);
                pre_ = saved;
                if (!pre.empty()) throw Unsupported{"this loop condition needs setup code"};
                out.close("until not " + cond);
                return;
            }
            case HxK::For: {
                forLoop(n, out);
                return;
            }
            case HxK::Return: {
                std::string value;
                if (n->a) {
                    Val v = ev(n->a);
                    if (!v.none) value = rd(v);
                }
                if (inCallback_ && cancelable_) {
                    value = "hx_stop and Function_Stop or nil";
                    cancelUsed_ = true;
                }
                std::string text = "return" + (value.empty() ? "" : " " + value);
                out.line(last ? text : "do " + text + " end");
                return;
            }
            case HxK::Break:
                out.line(last ? "break" : "do break end");
                return;
            case HxK::Continue: {
                if (continueLabels_.empty()) throw Unsupported{"continue outside a loop"};
                continueUsed_.back() = true;
                out.line("goto " + continueLabels_.back());
                return;
            }
            case HxK::Throw:
                out.line("error(" + L(n->a) + ")");
                return;
            case HxK::Switch: {
                switchStatement(n, out);
                return;
            }
            case HxK::Try: {
                bool hasReturn = false;
                walk(n->b, [&](const HxPtr& x) {
                    if (x->k == HxK::Return) hasReturn = true;
                });
                if (hasReturn) throw Unsupported{"return inside try has no Lua equivalent"};
                std::string fn = function({}, n->b, {});
                if (n->catches.empty()) {
                    out.line("pcall(" + fn + ")");
                    return;
                }
                counter_++;
                std::string err = "hx_err" + std::to_string(counter_);
                out.line("local hx_ok" + std::to_string(counter_) + ", " + err + " = pcall(" + fn + ")");
                out.open("if not hx_ok" + std::to_string(counter_) + " then");
                pushScope();
                Sym& s = declare(n->catches[0].name);
                out.line("local " + s.lua + " = " + err);
                block(n->catches[0].body, out);
                popScope();
                out.close();
                return;
            }
            default:
                break;
        }
        Val v = ev(n);
        if (v.none) return;
        if (v.k == VK::Obj) return;
        if (!v.tagKind.empty() && !v.call) return;
        std::string code = rd(v);
        if (code.empty()) return;
        if (v.call) {
            out.line(code);
            return;
        }
        out.line("local _ = " + code);
    }

    void body(const HxPtr& n, Out& out) {
        if (!n) return;
        if (n->k == HxK::Block) block(n->list, out);
        else statement(n, out, true);
    }

    void loopBody(const HxPtr& n, Out& out, const std::function<void()>& declareVars) {
        counter_++;
        std::string label = "hx_continue" + std::to_string(counter_);
        continueLabels_.push_back(label);
        continueUsed_.push_back(false);
        loopDepth_++;
        pushScope();
        if (declareVars) declareVars();
        Out inner;
        inner.depth = out.depth + 1;
        body(n, inner);
        popScope();
        loopDepth_--;
        bool used = continueUsed_.back();
        continueLabels_.pop_back();
        continueUsed_.pop_back();
        if (used) {
            out.open("do");
            out.append(inner);
            out.close();
            out.line("::" + label + "::");
        } else {
            for (const std::string& l : inner.lines) out.lines.push_back(l.size() >= 2 ? l.substr(2) : l);
        }
    }

    void forLoop(const HxPtr& n, Out& out) {
        const HxPtr& it = n->a;
        HxPtr range = it;
        while (range->k == HxK::Paren) range = range->a;
        if (n->names.size() == 1 && range->k == HxK::Binop && range->s == "...") {
            std::string from = L(range->a);
            std::string to = L(range->b, P_ADD);
            std::string var = safe(n->names[0]);
            out.open("for " + var + " = " + from + ", " + to + " - 1 do");
            loopBody(n->b, out, [&] {
                Sym& s = declare(n->names[0]);
                s.lua = var;
            });
            out.close();
            return;
        }
        if (n->names.size() == 2) {
            std::string m = L(it);
            std::string k = safe(n->names[0]);
            std::string v = safe(n->names[1]);
            out.open("for " + k + ", " + v + " in pairs(" + m + ") do");
            loopBody(n->b, out, [&] {
                declare(n->names[0]).lua = k;
                declare(n->names[1]).lua = v;
            });
            out.close();
            return;
        }
        if (range->k == HxK::Call && range->a->k == HxK::Field && range->a->s == "keys" && range->list.empty()) {
            std::string m = L(range->a->a);
            std::string k = safe(n->names[0]);
            out.open("for " + k + " in pairs(" + m + ") do");
            loopBody(n->b, out, [&] { declare(n->names[0]).lua = k; });
            out.close();
            return;
        }
        Val coll = ev(it);
        if (coll.k != VK::Lua) throw Unsupported{"only Lua tables can be looped over in Lua"};
        std::string var = safe(n->names[0]);
        if (coll.map) {
            out.open("for _, " + var + " in pairs(" + rd(coll) + ") do");
        } else {
            out.open("for _, " + var + " in ipairs(" + rd(coll) + ") do");
        }
        bool spr = coll.sprArr;
        loopBody(n->b, out, [&] {
            Sym& s = declare(n->names[0]);
            s.lua = var;
            s.spr = spr;
        });
        out.close();
    }

    void switchStatement(const HxPtr& n, Out& out) {
        for (const HxCase& c : n->cases) {
            for (const HxPtr& v : c.values) {
                if (v->k == HxK::Call || v->k == HxK::ArrayLit || v->k == HxK::ObjLit) throw Unsupported{"pattern matching has no Lua equivalent"};
                if (v->k == HxK::Ident && v->s != "_" && !std::isupper(static_cast<unsigned char>(v->s[0])) && !findSym(v->s) && !fields_.count(v->s))
                    throw Unsupported{"capture patterns have no Lua equivalent"};
            }
        }
        counter_++;
        std::string sw = "hx_sw" + std::to_string(counter_);
        out.line("local " + sw + " = " + L(n->a));
        bool first = true;
        const HxCase* fallbackCase = nullptr;
        for (const HxCase& c : n->cases) {
            bool wildcard = c.isDefault;
            for (const HxPtr& v : c.values) if (v->k == HxK::Ident && v->s == "_") wildcard = true;
            if (wildcard && !c.guard) {
                fallbackCase = &c;
                continue;
            }
            std::string cond;
            for (const HxPtr& v : c.values) {
                if (v->k == HxK::Ident && v->s == "_") continue;
                if (!cond.empty()) cond += " or ";
                cond += sw + " == " + L(v, P_CMP + 1);
            }
            if (c.guard) cond = (cond.empty() ? "" : "(" + cond + ") and ") + L(c.guard, P_AND + 1);
            if (cond.empty()) cond = "true";
            if (first) out.open("if " + cond + " then");
            else {
                out.depth--;
                out.open("elseif " + cond + " then");
            }
            first = false;
            pushScope();
            block(c.body, out);
            popScope();
        }
        if (fallbackCase) {
            if (first) {
                out.open("do");
            } else {
                out.depth--;
                out.open("else");
            }
            pushScope();
            block(fallbackCase->body, out);
            popScope();
            first = false;
        }
        if (!first) out.close();
    }

    std::vector<CbSpec> specsFor(const std::string& name, const HxMember& m) {
        static const std::vector<std::string> note = {"id", "direction", "noteType", "isSustainNote"};
        std::vector<CbSpec> out;
        auto add = [&](const std::string& psych, std::vector<std::string> params, const std::string& event, bool cancel = false, int must = -1,
                       bool prepend = false) {
            CbSpec s;
            s.psych = psych;
            s.params = std::move(params);
            s.event = event;
            s.cancel = cancel;
            s.mustPress = must;
            s.prepend = prepend;
            out.push_back(s);
        };
        if (!codename_) {
            if (name == "onCreate" || name == "buildStage") add("onCreate", {}, "none");
            else if (name == "onUpdate") add("onUpdate", {"elapsed"}, "update");
            else if (name == "onBeatHit") add("onBeatHit", {}, "beat");
            else if (name == "onStepHit") add("onStepHit", {}, "beat");
            else if (name == "onCountdownStart") add("onStartCountdown", {}, "none", true);
            else if (name == "onCountdownStep") add("onCountdownTick", {"counter"}, "countdown");
            else if (name == "onCountdownEnd") add("onSongStart", {}, "none", false, -1, true);
            else if (name == "onSongStart") add("onSongStart", {}, "none");
            else if (name == "onSongEnd") add("onEndSong", {}, "none", true);
            else if (name == "onPause") add("onPause", {}, "none", true);
            else if (name == "onResume") add("onResume", {}, "none");
            else if (name == "onGameOver") add("onGameOver", {}, "none", true);
            else if (name == "onDestroy") add("onDestroy", {}, "none");
            else if (name == "onNoteHit") {
                add("goodNoteHit", note, "note", false, 1);
                add("opponentNoteHit", note, "note", false, 0);
            } else if (name == "onNoteMiss") add("noteMiss", note, "note", false, 1);
            else if (name == "onNoteIncoming") add("onSpawnNote", note, "note", false, -2);
            else if (name == "onNoteGhostMiss") add("noteMissPress", {"direction"}, "ghost");
            else if (name == "onSongEvent") add("onEvent", {"name", "value1", "value2", "strumTime"}, "event");
            else if (name == "handleEvent" && kind_ == "event") add("onEvent", {"name", "value1", "value2", "strumTime"}, "handle");
        } else {
            if (name == "create") add("onCreate", {}, "none");
            else if (name == "postCreate") add("onCreatePost", {}, "none");
            else if (name == "update") add("onUpdate", {"elapsed"}, "param:elapsed");
            else if (name == "postUpdate") add("onUpdatePost", {"elapsed"}, "param:elapsed");
            else if (name == "beatHit") add("onBeatHit", {}, "param:curBeat");
            else if (name == "stepHit") add("onStepHit", {}, "param:curStep");
            else if (name == "measureHit" || name == "sectionHit") add("onSectionHit", {}, "param:curSection");
            else if (name == "onSongStart") add("onSongStart", {}, "none");
            else if (name == "onSongEnd") add("onEndSong", {}, "none", true);
            else if (name == "onStartCountdown") add("onStartCountdown", {}, "none", true);
            else if (name == "onCountdown") add("onCountdownTick", {"counter"}, "countdown");
            else if (name == "onPlayerHit") add("goodNoteHit", note, "note", true, 1);
            else if (name == "onDadHit") add("opponentNoteHit", note, "note", true, 0);
            else if (name == "onNoteHit") {
                add("goodNoteHit", note, "note", true, 1);
                add("opponentNoteHit", note, "note", true, 0);
            } else if (name == "onPlayerMiss") add("noteMiss", note, "note", true, 1);
            else if (name == "onNoteCreation" || name == "onPostNoteCreation") add("onSpawnNote", note, "note", false, -2);
            else if (name == "onEvent") add("onEvent", {"name", "value1", "value2", "strumTime"}, "event");
            else if (name == "onGamePause" || name == "onPause") add("onPause", {}, "none", true);
            else if (name == "onGameOver") add("onGameOver", {}, "none", true);
            else if (name == "destroy") add("onDestroy", {}, "none");
            else if (name == "onSubstateOpen" || name == "onFocus" || name == "onFocusLost" || name == "draw" || name == "postDraw") {
                issue(m.line, "Codename callback " + name + " has no Psych equivalent and was dropped.");
                add("", {}, "drop");
            }
        }
        return out;
    }

    void setupEvent(const CbSpec& spec) {
        eventMap_.clear();
        eventKind_ = spec.event;
        mustPress_ = spec.mustPress;
        std::string must = spec.mustPress == 1 ? "true" : spec.mustPress == 0 ? "false" : "getPropertyFromGroup('notes', id, 'mustPress')";
        std::string strum = "getPropertyFromGroup('notes', id, 'strumTime')";
        if (spec.event == "update") eventMap_ = {{"elapsed", "elapsed"}};
        if (spec.event == "beat") eventMap_ = {{"beat", "curBeat"}, {"step", "curStep"}};
        if (spec.event == "countdown") eventMap_ = {{"step", "counter"}, {"swagCounter", "counter"}};
        if (spec.event == "ghost") eventMap_ = {{"dir", "direction"}, {"direction", "direction"}};
        if (spec.event == "note") {
            eventMap_ = {{"note.noteData.getDirection()", "direction"}, {"note.direction", "direction"}, {"note.noteData.data", "direction"},
                {"note.noteData", codename_ ? "direction" : "direction"}, {"direction", "direction"}, {"note.strumID", "direction"},
                {"note.noteData.kind", "noteType"}, {"note.kind", "noteType"}, {"note.noteType", "noteType"}, {"noteType", "noteType"},
                {"note.noteData.getMustHitNote()", must}, {"note.mustPress", must}, {"note.mustHit", must}, {"player", must},
                {"note.noteData.time", strum}, {"note.strumTime", strum}, {"note.isHoldNote", "isSustainNote"},
                {"note.isSustainNote", "isSustainNote"}, {"note.noteData.length", "getPropertyFromGroup('notes', id, 'sustainLength')"},
                {"note.sustainLength", "getPropertyFromGroup('notes', id, 'sustainLength')"}, {"isComboBreak", "false"},
                {"judgement", "getPropertyFromGroup('notes', id, 'rating')"}, {"rating", "getPropertyFromGroup('notes', id, 'rating')"},
                {"note.noteData.getDirectionName()", "({'left', 'down', 'up', 'right'})[direction + 1]"}};
        }
        if (spec.event == "event") {
            eventMap_ = {{"eventData.eventKind", "name"}, {"eventData.event", "name"}, {"eventData.time", "strumTime"}, {"eventData.value", "value1"},
                {"event.name", "name"}, {"event.time", "strumTime"}, {"event.params[0]", "value1"}, {"event.params[1]", "value2"},
                {"event.params[2]", "nil"}, {"name", "name"}};
        }
        if (spec.event == "handle") eventMap_ = {{"eventKind", "name"}, {"time", "strumTime"}, {"value", "value1"}};
    }

    void build() {
        std::vector<const HxMember*> members;
        for (const HxMember& m : script_.members) members.push_back(&m);
        for (const HxMember* mp : members) {
            const HxMember& m = *mp;
            if (!m.isFunction) continue;
            if (m.name == "new" || m.name == "getTitle" || m.name == "getEventSchema") continue;
            std::vector<CbSpec> specs = specsFor(m.name, m);
            if (!specs.empty()) {
                for (const CbSpec& spec : specs) {
                    if (spec.event == "drop") continue;
                    callbackBody(m, spec);
                }
                continue;
            }
            if (!codename_ && kVSliceCallbacks.count(m.name)) {
                if (m.name == "fetchAssetPaths") issue(m.line, "fetchAssetPaths() has no Psych equivalent; Psych loads assets as they are used.");
                else issue(m.line, "V-Slice callback " + m.name + " has no Psych equivalent and was dropped.");
                continue;
            }
            helperMethod(m);
        }
        std::vector<std::string> createHead;
        Out init;
        init.depth = 1;
        pushScope();
        inCreate_ = true;
        inCallback_ = true;
        for (const std::string& name : fieldOrder_) {
            const HxMember* member = nullptr;
            for (const HxMember& m : script_.members) if (!m.isFunction && m.name == name) member = &m;
            if (!member || !member->value) continue;
            if (isConstant(member->value)) {
                std::vector<std::string> pre;
                pre_ = &pre;
                try {
                    fieldInit_[name] = L(member->value);
                    pre_ = nullptr;
                    continue;
                } catch (const Unsupported&) {
                }
                pre_ = nullptr;
            }
            HxPtr assign = std::make_shared<HxNode>();
            assign->k = HxK::Assign;
            assign->s = "=";
            assign->line = member->line;
            assign->start = member->value->start;
            assign->end = member->value->end;
            HxPtr target = std::make_shared<HxNode>();
            target->k = HxK::Ident;
            target->s = name;
            target->line = member->line;
            assign->a = target;
            assign->b = member->value;
            statement(assign, init, false);
        }
        for (const HxMember& m : script_.members) {
            if (m.name != "new" || !m.isFunction) continue;
            for (const HxPtr& s : m.body) {
                if (s->k == HxK::Call && s->a && s->a->k == HxK::Super) continue;
                statement(s, init, false);
            }
        }
        for (const HxPtr& s : script_.topLevel) statement(s, init, false);
        inCreate_ = false;
        inCallback_ = false;
        popScope();
        if (!init.lines.empty() || usesSelf_ || kind_ == "character") {
            Callback& cb = callbackFor("onCreate", {});
            std::vector<std::string> lines;
            if (kind_ == "character") {
                usesSelf_ = true;
                lines.push_back("  hx_self = 'boyfriend'");
                lines.push_back("  if getProperty('dad.curCharacter') == " + luaStr(targetId_) + " then");
                lines.push_back("    hx_self = 'dad'");
                lines.push_back("  elseif getProperty('gf.curCharacter') == " + luaStr(targetId_) + " then");
                lines.push_back("    hx_self = 'gf'");
                lines.push_back("  end");
            }
            for (const std::string& l : init.lines) lines.push_back(l);
            cb.bodies.insert(cb.bodies.begin(), lines);
        }
    }

    bool isConstant(const HxPtr& n) {
        bool ok = true;
        walk(n, [&](const HxPtr& x) {
            switch (x->k) {
                case HxK::Null: case HxK::True: case HxK::False: case HxK::Int: case HxK::Float: case HxK::ArrayLit: case HxK::MapLit:
                case HxK::ObjLit: case HxK::Paren: case HxK::Unop: case HxK::Binop:
                    break;
                case HxK::Str:
                    if (!x->parts.empty()) ok = false;
                    break;
                case HxK::New:
                    if (lastSegment(x->s) != "Map" && lastSegment(x->s) != "StringMap" && lastSegment(x->s) != "IntMap" && lastSegment(x->s) != "Array") ok = false;
                    break;
                default:
                    ok = false;
            }
        });
        return ok;
    }

    Callback& callbackFor(const std::string& name, const std::vector<std::string>& params) {
        auto it = callbacks_.find(name);
        if (it != callbacks_.end()) return it->second;
        Callback& cb = callbacks_[name];
        cb.name = name;
        cb.params = params;
        order_.push_back(name);
        return cb;
    }

    void callbackBody(const HxMember& m, const CbSpec& spec) {
        setupEvent(spec);
        cancelable_ = spec.cancel;
        cancelUsed_ = false;
        inCallback_ = true;
        inCreate_ = spec.psych == "onCreate" || spec.psych == "onCreatePost";
        pushScope();
        if (!m.params.empty()) {
            const std::string& p = m.params[0].name;
            Sym& s = scopes_.back()[p];
            s = Sym();
            if (spec.event.compare(0, 6, "param:") == 0) {
                s.lua = spec.event.substr(6);
            } else {
                s.kind = "event";
            }
        }
        Out o;
        o.depth = 1;
        if (spec.event == "note" && (kind_ == "notekind" || !opt_.noteType.empty()))
            o.line("if noteType ~= " + luaStr(opt_.noteType.empty() ? targetId_ : opt_.noteType) + " then return end");
        if (spec.event == "event" && !opt_.eventName.empty()) o.line("if name ~= " + luaStr(opt_.eventName) + " then return end");
        if (spec.event == "handle") o.line("if name ~= " + luaStr(targetId_) + " then return end");
        block(m.body, o);
        popScope();
        std::vector<std::string> lines;
        if (cancelUsed_) lines.push_back("  local hx_stop = false");
        for (const std::string& l : o.lines) lines.push_back(l);
        if (cancelUsed_) {
            bool endsWithReturn = !lines.empty() && lines.back().find("return hx_stop") != std::string::npos;
            if (!endsWithReturn) lines.push_back("  if hx_stop then return Function_Stop end");
        }
        Callback& cb = callbackFor(spec.psych, spec.params);
        if (spec.prepend) cb.bodies.insert(cb.bodies.begin(), lines);
        else cb.bodies.push_back(lines);
        inCallback_ = false;
        inCreate_ = false;
        cancelable_ = false;
        eventMap_.clear();
        eventKind_.clear();
    }

    void helperMethod(const HxMember& m) {
        std::string name = methods_[m.name];
        std::string fn = function(m.params, [&] {
            HxPtr b = std::make_shared<HxNode>();
            b->k = HxK::Block;
            b->list = m.body;
            return b;
        }(), {});
        methodText_.push_back(name + " = " + fn);
    }

    std::string helperSource(const std::string& name) {
        static const std::map<std::string, std::string> code = {
            {"hx_uid", "local hx_count = 0\nlocal function hx_uid()\n  hx_count = hx_count + 1\n  return hx_count\nend"},
            {"hx_int", "local function hx_int(x)\n  if x >= 0 then return math.floor(x) end\n  return math.ceil(x)\nend"},
            {"hx_indexOf", "local function hx_indexOf(a, x)\n  if type(a) == 'string' then\n    local i = string.find(a, x, 1, true)\n    return i and i - 1 or -1\n  end\n  for i = 1, #a do\n    if a[i] == x then return i - 1 end\n  end\n  return -1\nend"},
            {"hx_lastIndexOf", "local function hx_lastIndexOf(a, x)\n  if type(a) == 'string' then\n    local last = -1\n    local i = string.find(a, x, 1, true)\n    while i do\n      last = i - 1\n      i = string.find(a, x, i + 1, true)\n    end\n    return last\n  end\n  for i = #a, 1, -1 do\n    if a[i] == x then return i - 1 end\n  end\n  return -1\nend"},
            {"hx_contains", "local function hx_contains(a, x)\n  return hx_indexOf(a, x) >= 0\nend"},
            {"hx_remove", "local function hx_remove(a, x)\n  for i = 1, #a do\n    if a[i] == x then\n      table.remove(a, i)\n      return true\n    end\n  end\n  return false\nend"},
            {"hx_split", "local function hx_split(s, sep)\n  local out = {}\n  if sep == '' then\n    for i = 1, #s do out[#out + 1] = string.sub(s, i, i) end\n    return out\n  end\n  local start = 1\n  while true do\n    local i, j = string.find(s, sep, start, true)\n    if not i then\n      out[#out + 1] = string.sub(s, start)\n      return out\n    end\n    out[#out + 1] = string.sub(s, start, i - 1)\n    start = j + 1\n  end\nend"},
            {"hx_substr", "local function hx_substr(s, pos, len)\n  if pos < 0 then pos = math.max(#s + pos, 0) end\n  if len == nil then return string.sub(s, pos + 1) end\n  if len < 0 then return string.sub(s, pos + 1, #s + len) end\n  return string.sub(s, pos + 1, pos + len)\nend"},
            {"hx_substring", "local function hx_substring(s, a, b)\n  if b == nil then b = #s end\n  a = math.max(a, 0)\n  b = math.max(b, 0)\n  if a > b then a, b = b, a end\n  return string.sub(s, a + 1, b)\nend"},
            {"hx_replace", "local function hx_replace(s, from, to)\n  if from == '' then return s end\n  local out = ''\n  local start = 1\n  while true do\n    local i, j = string.find(s, from, start, true)\n    if not i then return out .. string.sub(s, start) end\n    out = out .. string.sub(s, start, i - 1) .. to\n    start = j + 1\n  end\nend"},
            {"hx_trim", "local function hx_trim(s)\n  return (string.gsub(s, '^%s*(.-)%s*$', '%1'))\nend"},
            {"hx_ltrim", "local function hx_ltrim(s)\n  return (string.gsub(s, '^%s*', ''))\nend"},
            {"hx_rtrim", "local function hx_rtrim(s)\n  return (string.gsub(s, '%s*$', ''))\nend"},
            {"hx_startsWith", "local function hx_startsWith(s, p)\n  return string.sub(s, 1, #p) == p\nend"},
            {"hx_endsWith", "local function hx_endsWith(s, p)\n  return p == '' or string.sub(s, -#p) == p\nend"},
            {"hx_lpad", "local function hx_lpad(s, c, l)\n  while #s < l do s = c .. s end\n  return s\nend"},
            {"hx_rpad", "local function hx_rpad(s, c, l)\n  while #s < l do s = s .. c end\n  return s\nend"},
            {"hx_copy", "local function hx_copy(a)\n  local out = {}\n  for k, v in pairs(a) do out[k] = v end\n  return out\nend"},
            {"hx_concat", "local function hx_concat(a, b)\n  local out = {}\n  for i = 1, #a do out[#out + 1] = a[i] end\n  for i = 1, #b do out[#out + 1] = b[i] end\n  return out\nend"},
            {"hx_slice", "local function hx_slice(a, s, e)\n  local out = {}\n  if e == nil then e = #a end\n  if s < 0 then s = #a + s end\n  if e < 0 then e = #a + e end\n  for i = s + 1, e do out[#out + 1] = a[i] end\n  return out\nend"},
            {"hx_splice", "local function hx_splice(a, pos, len)\n  local out = {}\n  if pos < 0 then pos = #a + pos end\n  for _ = 1, len do\n    if a[pos + 1] == nil then break end\n    out[#out + 1] = table.remove(a, pos + 1)\n  end\n  return out\nend"},
            {"hx_reverse", "local function hx_reverse(a)\n  local n = #a\n  for i = 1, math.floor(n / 2) do a[i], a[n - i + 1] = a[n - i + 1], a[i] end\nend"},
            {"hx_filter", "local function hx_filter(a, f)\n  local out = {}\n  for i = 1, #a do\n    if f(a[i]) then out[#out + 1] = a[i] end\n  end\n  return out\nend"},
            {"hx_map", "local function hx_map(a, f)\n  local out = {}\n  for i = 1, #a do out[i] = f(a[i]) end\n  return out\nend"},
            {"hx_hex", "local function hx_hex(c)\n  if type(c) == 'string' then return (string.gsub(c, '^#', '')) end\n  return string.format('%06X', c % 16777216)\nend"},
            {"hx_rgb", "local function hx_rgb(r, g, b, a)\n  return getColorFromHex(string.format('%02X%02X%02X', r, g, b))\nend"},
            {"hx_coalesce", "local function hx_coalesce(a, b)\n  if a == nil then return b end\n  return a\nend"},
            {"hx_bound", "local function hx_bound(v, lo, hi)\n  if lo ~= nil and v < lo then v = lo end\n  if hi ~= nil and v > hi then v = hi end\n  return v\nend"},
            {"hx_roundDecimal", "local function hx_roundDecimal(v, p)\n  local m = 10 ^ p\n  return math.floor(v * m + 0.5) / m\nend"},
            {"hx_remap", "local function hx_remap(v, a1, b1, a2, b2)\n  return a2 + (v - a1) * (b2 - a2) / (b1 - a1)\nend"},
            {"hx_keys", "local function hx_keys(t)\n  local out = {}\n  for k in pairs(t) do out[#out + 1] = k end\n  return out\nend"},
            {"hx_clear", "local function hx_clear(t)\n  for k in pairs(t) do t[k] = nil end\nend"},
            {"hx_set", "local function hx_set(t, k, v)\n  t[k] = v\nend"},
            {"hx_mapRemove", "local function hx_mapRemove(t, k)\n  local had = t[k] ~= nil\n  t[k] = nil\n  return had\nend"}};
        auto it = code.find(name);
        return it == code.end() ? "" : it->second;
    }

    std::string assemble() {
        std::vector<std::string> out;
        auto push = [&](const std::string& text) {
            size_t start = 0;
            for (;;) {
                size_t nl = text.find('\n', start);
                out.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        };
        bool locals = fieldOrder_.size() < 150;
        bool anyField = false;
        if (usesSelf_) {
            push("local hx_self = 'boyfriend'");
            anyField = true;
        }
        for (const std::string& name : fieldOrder_) {
            const Sym& s = fields_[name];
            if (s.spr && s.lit) continue;
            auto init = fieldInit_.find(name);
            std::string line = (locals ? "local " : "") + s.lua;
            if (init != fieldInit_.end()) line += " = " + init->second;
            else if (!locals) line += " = nil";
            push(line);
            anyField = true;
        }
        if (tweens_) {
            push("local hx_tweens = {}");
            anyField = true;
        }
        if (timers_) {
            push("local hx_timers = {}");
            anyField = true;
        }
        std::vector<std::string> helperOrder = {"hx_uid", "hx_int", "hx_indexOf", "hx_lastIndexOf", "hx_contains", "hx_remove", "hx_split",
            "hx_substr", "hx_substring", "hx_replace", "hx_trim", "hx_ltrim", "hx_rtrim", "hx_startsWith", "hx_endsWith", "hx_lpad", "hx_rpad",
            "hx_copy", "hx_concat", "hx_slice", "hx_splice", "hx_reverse", "hx_filter", "hx_map", "hx_hex", "hx_rgb", "hx_coalesce", "hx_bound",
            "hx_roundDecimal", "hx_remap", "hx_keys", "hx_clear", "hx_set", "hx_mapRemove"};
        bool anyHelper = false;
        for (const std::string& h : helperOrder) {
            if (!helpers_.count(h)) continue;
            if (!anyHelper && anyField) out.emplace_back();
            if (anyHelper) out.emplace_back();
            push(helperSource(h));
            anyHelper = true;
        }
        if (!methodText_.empty()) {
            if (anyField || anyHelper) out.emplace_back();
            std::string names;
            for (const auto& m : methods_) {
                bool emitted = false;
                for (const std::string& t : methodText_) if (t.compare(0, m.second.size() + 3, m.second + " = ") == 0) emitted = true;
                if (!emitted) continue;
                if (!names.empty()) names += ", ";
                names += m.second;
            }
            push("local " + names);
            for (const std::string& t : methodText_) {
                out.emplace_back();
                push(t);
            }
        }
        for (const std::string& name : order_) {
            const Callback& cb = callbacks_[name];
            if (!out.empty()) out.emplace_back();
            std::string params;
            for (size_t i = 0; i < cb.params.size(); i++) params += (i ? ", " : "") + cb.params[i];
            out.push_back("function " + name + "(" + params + ")");
            if (cb.bodies.size() == 1) {
                for (const std::string& l : cb.bodies[0]) out.push_back(l);
            } else {
                for (const auto& b : cb.bodies) {
                    if (b.empty()) continue;
                    out.push_back("  do");
                    for (const std::string& l : b) out.push_back(l.empty() ? l : "  " + l);
                    out.push_back("  end");
                }
            }
            out.push_back("end");
        }
        if (tweens_) {
            if (!out.empty()) out.emplace_back();
            push("function hxTweenDone_" + uniq_ + "(tag)\n  local key = hx_tweens[tag] and tag or string.gsub(tag, '^tween_', '')\n  local f = hx_tweens[key]\n  if f ~= nil then\n    hx_tweens[key] = nil\n    f()\n  end\nend");
            if (tweenEvents_ && !callbacks_.count("onTweenCompleted")) {
                out.emplace_back();
                push("function onTweenCompleted(tag)\n  hxTweenDone_" + uniq_ + "(tag)\nend");
            }
        }
        if (timers_ && !callbacks_.count("onTimerCompleted")) {
            if (!out.empty()) out.emplace_back();
            push("function onTimerCompleted(tag, loops, loopsLeft)\n  local f = hx_timers[tag]\n  if f ~= nil then\n    if loopsLeft == 0 then hx_timers[tag] = nil end\n    f(tag, loops, loopsLeft)\n  end\nend");
        }
        std::string text;
        for (const std::string& l : out) {
            size_t endPos = l.find_last_not_of(' ');
            text += (endPos == std::string::npos ? "" : l.substr(0, endPos + 1)) + "\n";
        }
        return text;
    }
};

}

HaxeLuaResult convertHaxeToLua(const std::string& source, const HaxeLuaOptions& options) {
    Translator translator(source, options);
    return translator.run();
}

}
