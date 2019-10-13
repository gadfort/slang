//------------------------------------------------------------------------------
// StringMethods.cpp
// Built-in methods on strings.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#include "slang/binding/SystemSubroutine.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/SysFuncsDiags.h"

namespace slang::Builtins {

class StringLenMethod : public SimpleSystemSubroutine {
public:
    StringLenMethod(Compilation& comp) :
        SimpleSystemSubroutine("len", SubroutineKind::Function, 0, {}, comp.getIntType(), true) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto val = args[0]->eval(context);
        if (!val)
            return nullptr;

        return SVInt(32, val.str().length(), true);
    }
};

class StringPutcMethod : public SimpleSystemSubroutine {
public:
    StringPutcMethod(Compilation& comp) :
        SimpleSystemSubroutine("putc", SubroutineKind::Function, 2,
                               { &comp.getIntType(), &comp.getByteType() }, comp.getVoidType(),
                               true) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto strCv = args[0]->evalLValue(context);
        auto indexCv = args[1]->eval(context);
        auto charCv = args[2]->eval(context);
        if (!strCv || !indexCv || !charCv)
            return nullptr;

        const std::string& str = strCv.load().str();
        int32_t index = indexCv.integer().as<int32_t>().value();
        uint8_t c = charCv.integer().as<uint8_t>().value();

        if (c == 0 || index < 0 || size_t(index) >= str.length())
            return nullptr;

        strCv.selectRange({ index, index }).store(SVInt(8, c, false));
        return nullptr;
    }
};

class StringGetcMethod : public SimpleSystemSubroutine {
public:
    StringGetcMethod(Compilation& comp) :
        SimpleSystemSubroutine("getc", SubroutineKind::Function, 1, { &comp.getIntType() },
                               comp.getByteType(), true) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto strCv = args[0]->eval(context);
        auto indexCv = args[1]->eval(context);
        if (!strCv || !indexCv)
            return nullptr;

        const std::string& str = strCv.str();
        int32_t index = indexCv.integer().as<int32_t>().value();
        if (index < 0 || size_t(index) >= str.length())
            return SVInt(8, 0, false);

        return SVInt(8, uint64_t(str[size_t(index)]), false);
    }
};

class StringUpperLowerMethod : public SimpleSystemSubroutine {
public:
    StringUpperLowerMethod(Compilation& comp, const std::string& name, bool upper) :
        SimpleSystemSubroutine(name, SubroutineKind::Function, 0, {}, comp.getStringType(), true),
        upper(upper) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto val = args[0]->eval(context);
        if (!val)
            return nullptr;

        std::string& str = val.str();
        if (upper) {
            std::transform(str.begin(), str.end(), str.begin(),
                           [](unsigned char c) { return std::toupper(c); });
        }
        else {
            std::transform(str.begin(), str.end(), str.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }
        return val;
    }

private:
    bool upper;
};

class StringCompareMethod : public SimpleSystemSubroutine {
public:
    StringCompareMethod(Compilation& comp, const std::string& name, bool ignoreCase) :
        SimpleSystemSubroutine(name, SubroutineKind::Function, 1, { &comp.getStringType() },
                               comp.getIntType(), true),
        ignoreCase(ignoreCase) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto lhsCv = args[0]->eval(context);
        auto rhsCv = args[1]->eval(context);
        if (!lhsCv || !rhsCv)
            return nullptr;

        std::string& lhs = lhsCv.str();
        std::string& rhs = rhsCv.str();

        int result;
        if (ignoreCase) {
            // No convenient way to do this with standard lib functions :(
            const char* p1 = lhs.c_str();
            const char* p2 = rhs.c_str();
            while ((result = std::tolower(*p1) - std::tolower(*p2++)) == 0) {
                if (*p1++ == '\0')
                    break;
            }
        }
        else {
            result = lhs.compare(rhs);
            if (result < 0)
                result = -1;
            else if (result > 0)
                result = 1;
        }

        return SVInt(32, uint64_t(result), true);
    }

private:
    bool ignoreCase;
};

class StringSubstrMethod : public SimpleSystemSubroutine {
public:
    StringSubstrMethod(Compilation& comp) :
        SimpleSystemSubroutine("substr", SubroutineKind::Function, 2,
                               { &comp.getIntType(), &comp.getIntType() }, comp.getStringType(),
                               true) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto strCv = args[0]->eval(context);
        auto leftCv = args[1]->eval(context);
        auto rightCv = args[2]->eval(context);
        if (!strCv || !leftCv || !rightCv)
            return nullptr;

        const std::string& str = strCv.str();
        int32_t left = leftCv.integer().as<int32_t>().value();
        int32_t right = rightCv.integer().as<int32_t>().value();
        if (left < 0 || right < left || size_t(right) >= str.length())
            return ""s;

        int32_t count = right - left + 1;
        return str.substr(size_t(left), size_t(count));
    }
};

class StringAtoIMethod : public SimpleSystemSubroutine {
public:
    StringAtoIMethod(Compilation& comp, const std::string& name, int base) :
        SimpleSystemSubroutine(name, SubroutineKind::Function, 0, {}, comp.getIntegerType(), true),
        base(base) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto cv = args[0]->eval(context);
        if (!cv)
            return nullptr;

        std::string str = cv.str();
        str.erase(std::remove(str.begin(), str.end(), '_'), str.end());

        // TODO: use from_chars and disallow prefix
        long result = strtol(str.c_str(), nullptr, base);
        return SVInt(32, uint64_t(result), true);
    }

private:
    int base;
};

class StringAtoRealMethod : public SimpleSystemSubroutine {
public:
    StringAtoRealMethod(Compilation& comp) :
        SimpleSystemSubroutine("atoreal", SubroutineKind::Function, 0, {}, comp.getRealType(),
                               true) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto cv = args[0]->eval(context);
        if (!cv)
            return nullptr;

        std::string str = cv.str();
        str.erase(std::remove(str.begin(), str.end(), '_'), str.end());

        // TODO: use from_chars
        double result = strtod(str.c_str(), nullptr);
        return real_t(result);
    }
};

class StringItoAMethod : public SimpleSystemSubroutine {
public:
    StringItoAMethod(Compilation& comp, const std::string& name, LiteralBase base) :
        SimpleSystemSubroutine(name, SubroutineKind::Function, 1, { &comp.getIntegerType() },
                               comp.getVoidType(), true),
        base(base) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto strCv = args[0]->evalLValue(context);
        auto valCv = args[1]->eval(context);
        if (!strCv || !valCv)
            return nullptr;

        strCv.store(valCv.integer().toString(base, false));
        return nullptr;
    }

private:
    LiteralBase base;
};

class StringRealtoAMethod : public SimpleSystemSubroutine {
public:
    StringRealtoAMethod(Compilation& comp) :
        SimpleSystemSubroutine("realtoa", SubroutineKind::Function, 1, { &comp.getRealType() },
                               comp.getVoidType(), true) {}

    ConstantValue eval(EvalContext& context, const Args& args) const final {
        auto strCv = args[0]->evalLValue(context);
        auto valCv = args[1]->eval(context);
        if (!strCv || !valCv)
            return nullptr;

        // TODO: use std::to_chars
        double value = valCv.real();
        size_t sz = (size_t)snprintf(nullptr, 0, "%f", value);

        std::string result(sz + 1, '\0');
        snprintf(result.data(), sz + 1, "%f", value);
        result.pop_back();

        strCv.store(std::move(result));
        return nullptr;
    }
};

void registerStringMethods(Compilation& c) {
#define REGISTER(kind, name, ...) \
    c.addSystemMethod(kind, std::make_unique<name##Method>(__VA_ARGS__))
    REGISTER(SymbolKind::StringType, StringLen, c);
    REGISTER(SymbolKind::StringType, StringPutc, c);
    REGISTER(SymbolKind::StringType, StringGetc, c);
    REGISTER(SymbolKind::StringType, StringUpperLower, c, "toupper", true);
    REGISTER(SymbolKind::StringType, StringUpperLower, c, "tolower", false);
    REGISTER(SymbolKind::StringType, StringCompare, c, "compare", false);
    REGISTER(SymbolKind::StringType, StringCompare, c, "icompare", true);
    REGISTER(SymbolKind::StringType, StringSubstr, c);
    REGISTER(SymbolKind::StringType, StringAtoI, c, "atoi", 10);
    REGISTER(SymbolKind::StringType, StringAtoI, c, "atohex", 16);
    REGISTER(SymbolKind::StringType, StringAtoI, c, "atooct", 8);
    REGISTER(SymbolKind::StringType, StringAtoI, c, "atobin", 2);
    REGISTER(SymbolKind::StringType, StringAtoReal, c);
    REGISTER(SymbolKind::StringType, StringItoA, c, "itoa", LiteralBase::Decimal);
    REGISTER(SymbolKind::StringType, StringItoA, c, "hextoa", LiteralBase::Hex);
    REGISTER(SymbolKind::StringType, StringItoA, c, "octtoa", LiteralBase::Octal);
    REGISTER(SymbolKind::StringType, StringItoA, c, "bintoa", LiteralBase::Binary);
    REGISTER(SymbolKind::StringType, StringRealtoA, c);

#undef REGISTER
}

} // namespace slang::Builtins