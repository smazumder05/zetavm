#include <cassert>
#include <iostream>
#include <unordered_map>
#include "runtime.h"
#include "parser.h"
#include "interp.h"
#include "core.h"

/// Inline cache to speed up property lookups
class ICache
{
private:

    // Cached slot index
    size_t slotIdx = 0;

    // Field name to look up
    std::string fieldName;

public:

    ICache(std::string fieldName)
    : fieldName(fieldName)
    {
    }

    Value getField(Object obj)
    {
        Value val;

        if (!obj.getField(fieldName.c_str(), val, slotIdx))
        {
            throw RunError("missing field \"" + fieldName + "\"");
        }

        return val;
    }

    int64_t getInt64(Object obj)
    {
        auto val = getField(obj);
        assert (val.isInt64());
        return (int64_t)val;
    }

    String getStr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isString());
        return String(val);
    }

    Object getObj(Object obj)
    {
        auto val = getField(obj);
        assert (val.isObject());
        return Object(val);
    }

    Array getArr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isArray());
        return Array(val);
    }
};

std::string posToString(Value srcPos)
{
    assert (srcPos.isObject());
    auto srcPosObj = (Object)srcPos;

    auto lineNo = (int64_t)srcPosObj.getField("line_no");
    auto colNo = (int64_t)srcPosObj.getField("col_no");
    auto srcName = (std::string)srcPosObj.getField("src_name");

    return (
        srcName + "@" +
        std::to_string(lineNo) + ":" +
        std::to_string(colNo)
    );
}

/// Opcode enumeration
enum Opcode : uint16_t
{
    GET_LOCAL,
    SET_LOCAL,

    // Stack manipulation
    PUSH,
    POP,
    DUP,
    SWAP,

    // 64-bit integer operations
    ADD_I64,
    SUB_I64,
    MUL_I64,
    LT_I64,
    LE_I64,
    GT_I64,
    GE_I64,
    EQ_I64,

    // String operations
    STR_LEN,
    GET_CHAR,
    GET_CHAR_CODE,
    STR_CAT,
    EQ_STR,

    // Object operations
    NEW_OBJECT,
    HAS_FIELD,
    SET_FIELD,
    GET_FIELD,
    EQ_OBJ,

    // Miscellaneous
    EQ_BOOL,
    HAS_TAG,
    GET_TAG,

    // Array operations
    NEW_ARRAY,
    ARRAY_LEN,
    ARRAY_PUSH,
    GET_ELEM,
    SET_ELEM,

    // Branch instructions
    // Note: opcode for stub branches is opcode+1
    JUMP,
    JUMP_STUB,
    IF_TRUE,
    IF_TRUE_STUB,
    CALL,
    RET,

    IMPORT,
    ABORT
};

/// Map from pointers to instruction objects to opcodes
std::unordered_map<refptr, Opcode> opCache;

/// Total count of instructions executed
size_t cycleCount = 0;

/// Cache of all possible one-character string values
Value charStrings[256];

Opcode decode(Object instr)
{
    auto instrPtr = (refptr)instr;

    if (opCache.find(instrPtr) != opCache.end())
    {
        //std::cout << "cache hit" << std::endl;
        return opCache[instrPtr];
    }

    // Get the opcode string for this instruction
    static ICache opIC("op");
    auto opStr = (std::string)opIC.getStr(instr);

    //std::cout << "decoding \"" << opStr << "\"" << std::endl;

    Opcode op;

    // Local variable access
    if (opStr == "get_local")
        op = GET_LOCAL;
    else if (opStr == "set_local")
        op = SET_LOCAL;

    // Stack manipulation
    else if (opStr == "push")
        op = PUSH;
    else if (opStr == "pop")
        op = POP;
    else if (opStr == "dup")
        op = DUP;
    else if (opStr == "push")
        op = PUSH;

    // 64-bit integer operations
    else if (opStr == "add_i64")
        op = ADD_I64;
    else if (opStr == "sub_i64")
        op = SUB_I64;
    else if (opStr == "mul_i64")
        op = MUL_I64;
    else if (opStr == "lt_i64")
        op = LT_I64;
    else if (opStr == "le_i64")
        op = LE_I64;
    else if (opStr == "gt_i64")
        op = GT_I64;
    else if (opStr == "ge_i64")
        op = GE_I64;
    else if (opStr == "eq_i64")
        op = EQ_I64;

    // String operations
    else if (opStr == "str_len")
        op = STR_LEN;
    else if (opStr == "get_char")
        op = GET_CHAR;
    else if (opStr == "get_char_code")
        op = GET_CHAR_CODE;
    else if (opStr == "str_cat")
        op = STR_CAT;
    else if (opStr == "eq_str")
        op = EQ_STR;

    // Object operations
    else if (opStr == "new_object")
        op = NEW_OBJECT;
    else if (opStr == "has_field")
        op = HAS_FIELD;
    else if (opStr == "set_field")
        op = SET_FIELD;
    else if (opStr == "get_field")
        op = GET_FIELD;
    else if (opStr == "eq_obj")
        op = EQ_OBJ;

    // Array operations
    else if (opStr == "new_array")
        op = NEW_ARRAY;
    else if (opStr == "array_len")
        op = ARRAY_LEN;
    else if (opStr == "array_push")
        op = ARRAY_PUSH;
    else if (opStr == "get_elem")
        op = GET_ELEM;
    else if (opStr == "set_elem")
        op = SET_ELEM;

    // Miscellaneous
    else if (opStr == "eq_bool")
        op = EQ_BOOL;
    else if (opStr == "has_tag")
        op = HAS_TAG;

    // Branch instructions
    else if (opStr == "jump")
        op = JUMP;
    else if (opStr == "if_true")
        op = IF_TRUE;
    else if (opStr == "call")
        op = CALL;
    else if (opStr == "ret")
        op = RET;

    // VM interface
    else if (opStr == "import")
        op = IMPORT;
    else if (opStr == "abort")
        op = ABORT;

    else
        throw RunError("unknown op in decode \"" + opStr + "\"");

    opCache[instrPtr] = op;
    return op;
}

Value call(Object fun, ValueVec args)
{
    static ICache numParamsIC("num_params");
    static ICache numLocalsIC("num_locals");
    auto numParams = numParamsIC.getInt64(fun);
    auto numLocals = numLocalsIC.getInt64(fun);
    assert (args.size() <= numParams);
    assert (numParams <= numLocals);

    ValueVec locals;
    locals.resize(numLocals, Value::UNDEF);

    // Copy the arguments into the locals
    for (size_t i = 0; i < args.size(); ++i)
    {
        //std::cout << "  " << args[i].toString() << std::endl;
        locals[i] = args[i];
    }

    // Temporary value stack
    ValueVec stack;

    // Array of instructions to execute
    Value instrs;

    // Number of instructions in the current block
    size_t numInstrs = 0;

    // Index of the next instruction to execute
    size_t instrIdx = 0;

    auto popVal = [&stack]()
    {
        if (stack.empty())
            throw RunError("op cannot pop value, stack empty");
        auto val = stack.back();
        stack.pop_back();
        return val;
    };

    auto popBool = [&popVal]()
    {
        auto val = popVal();
        if (!val.isBool())
            throw RunError("op expects boolean value");
        return (bool)val;
    };

    auto popInt64 = [&popVal]()
    {
        auto val = popVal();
        if (!val.isInt64())
            throw RunError("op expects int64 value");
        return (int64_t)val;
    };

    auto popStr = [&popVal]()
    {
        auto val = popVal();
        if (!val.isString())
            throw RunError("op expects string value");
        return String(val);
    };

    auto popArray = [&popVal]()
    {
        auto val = popVal();
        if (!val.isArray())
            throw RunError("op expects array value");
        return Array(val);
    };

    auto popObj = [&popVal]()
    {
        auto val = popVal();
        assert (val.isObject());
        return Object(val);
    };

    auto pushBool = [&stack](bool val)
    {
        stack.push_back(val? Value::TRUE:Value::FALSE);
    };

    auto branchTo = [&instrs, &numInstrs, &instrIdx](Object targetBB)
    {
        //std::cout << "branching" << std::endl;

        if (instrIdx != numInstrs)
        {
            throw RunError(
                "only the last instruction in a block can be a branch ("
                "instrIdx=" + std::to_string(instrIdx) + ", " +
                "numInstrs=" + std::to_string(numInstrs) + ")"
            );
        }

        static ICache instrsIC("instrs");
        Array instrArr = instrsIC.getArr(targetBB);

        instrs = (Value)instrArr;
        numInstrs = instrArr.length();
        instrIdx = 0;

        if (numInstrs == 0)
        {
            throw RunError("target basic block is empty");
        }
    };

    // Get the entry block for this function
    static ICache entryIC("entry");
    Object entryBB = entryIC.getObj(fun);

    // Branch to the entry block
    branchTo(entryBB);

    // For each instruction to execute
    for (;;)
    {
        assert (instrIdx < numInstrs);

        //std::cout << "cycleCount=" << cycleCount << std::endl;
        //std::cout << "instrIdx=" << instrIdx << std::endl;

        Array instrArr = Array(instrs);
        Value instrVal = instrArr.getElem(instrIdx);
        assert (instrVal.isObject());
        auto instr = Object(instrVal);

        cycleCount++;
        instrIdx++;

        // Get the opcode for this instruction
        auto op = decode(instr);

        switch (op)
        {
            // Read a local variable and push it on the stack
            case GET_LOCAL:
            {
                static ICache icache("idx");
                auto localIdx = icache.getInt64(instr);
                //std::cout << "localIdx=" << localIdx << std::endl;
                assert (localIdx < locals.size());
                stack.push_back(locals[localIdx]);
            }
            break;

            // Set a local variable
            case SET_LOCAL:
            {
                static ICache icache("idx");
                auto localIdx = icache.getInt64(instr);
                //std::cout << "localIdx=" << localIdx << std::endl;
                assert (localIdx < locals.size());
                locals[localIdx] = popVal();
            }
            break;

            case PUSH:
            {
                static ICache icache("val");
                auto val = icache.getField(instr);
                stack.push_back(val);
            }
            break;

            case POP:
            {
                if (stack.empty())
                    throw RunError("pop failed, stack empty");
                stack.pop_back();
            }
            break;

            // Duplicate the top of the stack
            case DUP:
            {
                static ICache idxIC("idx");
                auto idx = idxIC.getInt64(instr);

                if (idx >= stack.size())
                    throw RunError("stack undeflow, invalid index for dup");

                auto val = stack[stack.size() - 1 - idx];
                stack.push_back(val);
            }
            break;

            // Swap the topmost two stack elements
            case SWAP:
            {
                auto v0 = popVal();
                auto v1 = popVal();
                stack.push_back(v0);
                stack.push_back(v1);
            }
            break;

            //
            // 64-bit integer operations
            //

            case ADD_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 + arg1);
            }
            break;

            case SUB_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 - arg1);
            }
            break;

            case MUL_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 * arg1);
            }
            break;

            case LT_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 < arg1);
            }
            break;

            case LE_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 <= arg1);
            }
            break;

            case GT_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 > arg1);
            }
            break;

            case GE_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 >= arg1);
            }
            break;

            case EQ_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // String operations
            //

            case STR_LEN:
            {
                auto str = popStr();
                stack.push_back(str.length());
            }
            break;

            case GET_CHAR:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char, index out of bounds"
                    );
                }

                auto ch = str[idx];

                // Cache single-character strings
                if (charStrings[ch] == Value::FALSE)
                {
                    char buf[2] = { (char)str[idx], '\0' };
                    charStrings[ch] = String(buf);
                }

                stack.push_back(charStrings[ch]);
            }
            break;

            case GET_CHAR_CODE:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char_code, index out of bounds"
                    );
                }

                stack.push_back((int64_t)str[idx]);
            }
            break;

            case STR_CAT:
            {
                auto a = popStr();
                auto b = popStr();
                auto c = String::concat(b, a);
                stack.push_back(c);
            }
            break;

            case EQ_STR:
            {
                auto arg1 = popStr();
                auto arg0 = popStr();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Object operations
            //

            case NEW_OBJECT:
            {
                auto capacity = popInt64();
                auto obj = Object::newObject(capacity);
                stack.push_back(obj);
            }
            break;

            case HAS_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();
                pushBool(obj.hasField(fieldName));
            }
            break;

            case SET_FIELD:
            {
                auto val = popVal();
                auto fieldName = popStr();
                auto obj = popObj();

                if (!isValidIdent(fieldName))
                {
                    throw RunError(
                        "invalid identifier in set_field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                obj.setField(fieldName, val);
            }
            break;

            // This instruction will abort execution if trying to
            // access a field that is not present on an object.
            // The running program is responsible for testing that
            // fields exist before attempting to read them.
            case GET_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();

                //std::cout << "get " << std::string(fieldName) << std::endl;

                if (!obj.hasField(fieldName))
                {
                    throw RunError(
                        "get_field failed, missing field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                auto val = obj.getField(fieldName);
                stack.push_back(val);
            }
            break;

            case EQ_OBJ:
            {
                Value arg1 = popVal();
                Value arg0 = popVal();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Array operations
            //

            case NEW_ARRAY:
            {
                auto len = popInt64();
                auto array = Array(len);
                stack.push_back(array);
            }
            break;

            case ARRAY_LEN:
            {
                auto arr = Array(popVal());
                stack.push_back(arr.length());
            }
            break;

            case ARRAY_PUSH:
            {
                auto val = popVal();
                auto arr = Array(popVal());
                arr.push(val);
            }
            break;

            case SET_ELEM:
            {
                auto val = popVal();
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "set_elem, index out of bounds"
                    );
                }

                arr.setElem(idx, val);
            }
            break;

            case GET_ELEM:
            {
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "get_elem, index out of bounds"
                    );
                }

                stack.push_back(arr.getElem(idx));
            }
            break;

            case EQ_BOOL:
            {
                auto arg1 = popBool();
                auto arg0 = popBool();
                pushBool(arg0 == arg1);
            }
            break;

            // Test if a value has a given tag
            case HAS_TAG:
            {
                auto tag = popVal().getTag();
                static ICache tagIC("tag");
                auto tagStr = tagIC.getStr(instr);

                switch (tag)
                {
                    case TAG_UNDEF:
                    pushBool(tagStr == "undef");
                    break;

                    case TAG_BOOL:
                    pushBool(tagStr == "bool");
                    break;

                    case TAG_INT64:
                    pushBool(tagStr == "int64");
                    break;

                    case TAG_STRING:
                    pushBool(tagStr == "string");
                    break;

                    case TAG_ARRAY:
                    pushBool(tagStr == "array");
                    break;

                    case TAG_OBJECT:
                    pushBool(tagStr == "object");
                    break;

                    default:
                    throw RunError(
                        "unknown value type in has_tag"
                    );
                }
            }
            break;

            case JUMP:
            {
                static ICache icache("to");
                auto target = icache.getObj(instr);
                branchTo(target);
            }
            break;

            case IF_TRUE:
            {
                static ICache thenCache("then");
                static ICache elseCache("else");
                auto thenBB = thenCache.getObj(instr);
                auto elseBB = elseCache.getObj(instr);
                auto arg0 = popVal();
                branchTo((arg0 == Value::TRUE)? thenBB:elseBB);
            }
            break;

            // Regular function call
            case CALL:
            {
                static ICache retToCache("ret_to");
                static ICache numArgsCache("num_args");
                auto retToBB = retToCache.getObj(instr);
                auto numArgs = numArgsCache.getInt64(instr);

                auto callee = popVal();

                if (stack.size() < numArgs)
                {
                    throw RunError(
                        "stack underflow at call"
                    );
                }

                // Copy the arguments into a vector
                ValueVec args;
                args.resize(numArgs);
                for (size_t i = 0; i < numArgs; ++i)
                    args[numArgs - 1 - i] = popVal();

                static ICache numParamsIC("num_params");
                size_t numParams;
                if (callee.isObject())
                {
                    numParams = numParamsIC.getInt64(callee);
                }
                else if (callee.isHostFn())
                {
                    auto hostFn = (HostFn*)(callee.getWord().ptr);
                    numParams = hostFn->getNumParams();
                }
                else
                {
                    throw RunError("invalid callee at call site");
                }

                if (numArgs != numParams)
                {
                    std::string srcPosStr = (
                        instr.hasField("src_pos")?
                        (posToString(instr.getField("src_pos")) + " - "):
                        std::string("")
                    );

                    throw RunError(
                        srcPosStr +
                        "incorrect argument count in call, received " +
                        std::to_string(numArgs) +
                        ", expected " +
                        std::to_string(numParams)
                    );
                }

                Value retVal;

                if (callee.isObject())
                {
                    // Perform the call
                    retVal = call(callee, args);
                }
                else if (callee.isHostFn())
                {
                    auto hostFn = (HostFn*)(callee.getWord().ptr);

                    // Call the host function
                    switch (numArgs)
                    {
                        case 0:
                        retVal = hostFn->call0();
                        break;

                        case 1:
                        retVal = hostFn->call1(args[0]);
                        break;

                        case 2:
                        retVal = hostFn->call2(args[0], args[1]);
                        break;

                        case 3:
                        retVal = hostFn->call3(args[0], args[1], args[2]);
                        break;

                        default:
                        assert (false);
                    }
                }

                // Push the return value on the stack
                stack.push_back(retVal);

                // Jump to the return basic block
                branchTo(retToBB);
            }
            break;

            case RET:
            {
                auto val = stack.back();
                stack.pop_back();
                return val;
            }
            break;

            case IMPORT:
            {
                auto pkgName = popStr();
                auto pkg = import(pkgName);
                stack.push_back(pkg);
            }
            break;

            case ABORT:
            {
                auto errMsg = (std::string)popStr();

                // If a source position was specified
                if (instr.hasField("src_pos"))
                {
                    auto srcPos = instr.getField("src_pos");
                    std::cout << posToString(srcPos) << " - ";
                }

                if (errMsg != "")
                {
                    std::cout << "aborting execution due to error: ";
                    std::cout << errMsg << std::endl;
                }
                else
                {
                    std::cout << "aborting execution due to error" << std::endl;
                }

                exit(-1);
            }
            break;

            default:
            assert (false && "unhandled op in interpreter");
        }
    }

    assert (false);
}

/// Call a function exported by a package
Value callExportFn(
    Object pkg,
    std::string fnName,
    ValueVec args
)
{
    assert (pkg.hasField(fnName));
    auto fnVal = pkg.getField(fnName);
    assert (fnVal.isObject());
    auto funObj = Object(fnVal);

    return call(funObj, args);
}

Value testRunImage(std::string fileName)
{
    std::cout << "loading image \"" << fileName << "\"" << std::endl;

    auto pkg = parseFile(fileName);

    return callExportFn(pkg, "main");
}

void testInterp()
{
    std::cout << "interpreter tests" << std::endl;

    assert (testRunImage("tests/zetavm/ex_ret_cst.zim") == Value(777));
    assert (testRunImage("tests/zetavm/ex_loop_cnt.zim") == Value(0));
    assert (testRunImage("tests/zetavm/ex_image.zim") == Value(10));
    assert (testRunImage("tests/zetavm/ex_rec_fact.zim") == Value(5040));
    assert (testRunImage("tests/zetavm/ex_fibonacci.zim") == Value(377));
}

//============================================================================
// New interpreter
//============================================================================

/// Initial code heap size in bytes
const size_t CODE_HEAP_INIT_SIZE = 1 << 20;

/// Initial stack size in words
const size_t STACK_INIT_SIZE = 1 << 16;

class CodeFragment
{
public:

    /// Start index in the executable heap
    uint8_t* startPtr = nullptr;

    /// End index in the executable heap
    uint8_t* endPtr = nullptr;

    /// Get the length of the code fragment
    size_t length()
    {
        assert (startPtr);
        assert (endPtr);
        return endPtr - startPtr;
    }

    /*
    /// Store the start position of the code
    void markStart(CodeBlock as)
    {
        assert (
            startIdx is startIdx.max,
            "start position is already marked"
        );

        startIdx = cast(uint32_t)as.getWritePos();

        // Add a label string comment
        if (opts.genasm)
            as.writeString(this.getName ~ ":");
    }
    */

    /*
    /// Store the end position of the code
    void markEnd(CodeBlock as)
    {
        assert (
            !ended,
            "end position is already marked"
        );

        endIdx = cast(uint32_t)as.getWritePos();
    }
    */
};

class BlockVersion : public CodeFragment
{
public:

    /// Associated block
    Object block;

    /// Code generation context at block entry
    //CodeGenCtx ctx;

    BlockVersion(Object block)
    : block(block)
    {
    }
};

typedef std::vector<BlockVersion*> VersionList;

/// Flat array of bytes into which code gets compiled
uint8_t* codeHeap = nullptr;

/// Limit pointer for the code heap
uint8_t* codeHeapLimit = nullptr;

/// Current allocation pointer in the code heap
uint8_t* codeHeapAlloc = nullptr;

/// Map of block objects to lists of versions
std::unordered_map<refptr, VersionList> versionMap;

/// Size of the stack in words
size_t stackSize = 0;

/// Lower stack limit (stack pointer must be greater than this)
Value* stackLimit = nullptr;

/// Stack bottom (end of the stack memory array)
Value* stackBottom = nullptr;

/// Stack frame base pointer
Value* basePtr = nullptr;

/// Current temp stack top pointer
Value* stackPtr = nullptr;

// Current instruction pointer
uint8_t* instrPtr = nullptr;

// Write a value to the code heap
template <typename T> void writeVal(T val)
{
    assert (codeHeapAlloc < codeHeapLimit);
    T* heapPtr = (T*)codeHeapAlloc;
    *heapPtr = val;
    codeHeapAlloc += sizeof(T);
    assert (codeHeapAlloc <= codeHeapLimit);
}

template <typename T> T readVal()
{
    assert (instrPtr + sizeof(T) <= codeHeapLimit);
    T* valPtr = (T*)instrPtr;
    auto val = *valPtr;
    instrPtr += sizeof(T);
    return val;
}

/// Initialize the interpreter
void initInterp()
{
    // Allocate the code heap
    codeHeap = new uint8_t[CODE_HEAP_INIT_SIZE];
    codeHeapLimit = codeHeap + CODE_HEAP_INIT_SIZE;
    codeHeapAlloc = codeHeap;

    // Allocate the stack
    stackSize = STACK_INIT_SIZE;
    stackLimit = new Value[STACK_INIT_SIZE];
    stackBottom = stackLimit + sizeof(Word);
    stackPtr = stackBottom;
}

// TODO: do we already need a versioning context?
// we need to manage the temp stack size?
// can technically just use the stack top for this?

/// Get a version of a block. This version will be a stub
/// until compiled
BlockVersion* getBlockVersion(Object block)
{
    auto blockPtr = (refptr)block;

    auto versionItr = versionMap.find((refptr)block);

    if (versionItr == versionMap.end())
    {
        versionMap[blockPtr] = VersionList();
    }
    else
    {
        auto versions = versionItr->second;
        assert (versions.size() == 1);
        return versions[0];
    }

    auto newVersion = new BlockVersion(block);

    auto& versionList = versionMap[blockPtr];
    versionList.push_back(newVersion);

    return newVersion;
}

void compile(BlockVersion* version)
{
    auto block = version->block;

    // Block name icache, for debugging
    static ICache nameIC("name");
    auto name = nameIC.getStr(block);

    // Get the instructions array
    static ICache instrsIC("instrs");
    Array instrs = instrsIC.getArr(block);

    // Mark the block start
    version->startPtr = codeHeapAlloc;

    // For each instruction
    for (size_t i = 0; i < instrs.length(); ++i)
    {
        auto instrVal = instrs.getElem(i);
        assert (instrVal.isObject());
        auto instr = (Object)instrVal;

        static ICache opIC("op");
        auto op = (std::string)opIC.getStr(instr);

        std::cout << "op: " << op << std::endl;

        if (op == "push")
        {
            static ICache valIC("val");
            auto val = valIC.getField(instr);
            writeVal(PUSH);
            writeVal(val);
            continue;
        }

        if (op == "ret")
        {
            writeVal(RET);
            continue;
        }

        throw RunError("unhandled opcode in basic block \"" + op + "\"");
    }

    // Mark the block end
    version->endPtr = codeHeapAlloc;
}

/// Push a value on the stack
void pushVal(Value val)
{
    stackPtr--;
    stackPtr[0] = val;
}

/// Start/continue execution beginning at a current instruction
Value execCode()
{
    assert (instrPtr >= codeHeap);
    assert (instrPtr < codeHeapLimit);

    // For each instruction to execute
    for (;;)
    {
        auto op = readVal<Opcode>();

        switch (op)
        {
            case PUSH:
            {
                auto val = readVal<Value>();

                // TODO
            }
            break;

            // TODO: stop when returning to null
            case RET:
            {
                // Pop val from stack



            }
            break;

            default:
            assert (false);
        }

    }

    assert (false);
}

/// Begin the execution of a function (top-level call)
Value callFun(Object fun, ValueVec args)
{
    static ICache numParamsIC("num_params");
    static ICache numLocalsIC("num_locals");
    auto numParams = numParamsIC.getInt64(fun);
    auto numLocals = numLocalsIC.getInt64(fun);
    assert (args.size() <= numParams);
    assert (numParams <= numLocals);

    std::cout << "pushing RA" << std::endl;

    // Push the caller function and return address
    // Note: these are placeholders because we are doing a toplevel call
    assert (stackPtr == stackBottom);
    pushVal(Value(0));
    pushVal(Value(nullptr, TAG_RETADDR));

    // Initialize the base pointer (used to access locals)
    basePtr = stackPtr - 1;

    // Push space for the local variables
    stackPtr -= numLocals;
    assert (stackPtr >= stackLimit);

    std::cout << "pushing locals" << std::endl;

    // Copy the arguments into the locals
    for (size_t i = 0; i < args.size(); ++i)
    {
        //std::cout << "  " << args[i].toString() << std::endl;
        basePtr[i] = args[i];
    }

    // Get the function entry block
    static ICache entryIC("entry");
    auto entryBlock = entryIC.getObj(fun);

    auto entryVer = getBlockVersion(entryBlock);

    // Generate code for the entry block version
    compile(entryVer);
    assert (entryVer->length() > 0);

    // Begin execution at the entry block
    //auto retVal = execCode(entryVer->startPtr);

    // Pop the local variables, return address and calling function
    stackPtr += numLocals;
    stackPtr += 2;
    assert (stackPtr == stackBottom);

    // TODO
    //return retVal;
    return Value(777);
}

/// Call a function exported by a package
Value callExportFnNew(
    Object pkg,
    std::string fnName,
    ValueVec args = ValueVec()
)
{
    assert (pkg.hasField(fnName));
    auto fnVal = pkg.getField(fnName);
    assert (fnVal.isObject());
    auto funObj = Object(fnVal);

    return callFun(funObj, args);
}

Value testRunImageNew(std::string fileName)
{
    std::cout << "loading image \"" << fileName << "\"" << std::endl;

    auto pkg = parseFile(fileName);

    return callExportFnNew(pkg, "main");
}

void testInterpNew()
{
    // TODO: call main function of simple test

    assert (testRunImageNew("tests/zetavm/ex_ret_cst.zim") == Value(777));
    //assert (testRunImageNew("tests/zetavm/ex_loop_cnt.zim") == Value(0));
    //assert (testRunImageNew("tests/zetavm/ex_image.zim") == Value(10));
    //assert (testRunImageNew("tests/zetavm/ex_rec_fact.zim") == Value(5040));
    //assert (testRunImageNew("tests/zetavm/ex_fibonacci.zim") == Value(377));
}
