#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"

VM vm; // VMはグローバル変数. clox の実行はこのグローバル変数のVMが行う.
static Value clockNative(int argCount, Value *args) {
  return NUMBER_VAL((double) clock() / CLOCKS_PER_SEC);
}

// resetStack はグローバル変数vmのスタックを初期化する
static void resetStack() {
  vm.stackTop = vm.stack; // スタックポインタを先頭に.
  vm.frameCount = 0;      // VM起動時は CallFrame は 0 である.
  vm.openUpvalues = NULL;
}

static void runtimeError(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

/*
  CallFrame* frame = &vm.frames[vm.frameCount - 1]; // CallFrameスタックの先頭を取得
  size_t instruction = frame->ip - frame->function->chunk.code - 1;
  int line = frame->function->chunk.lines[instruction];
*/
  // CallStackを呼び出し元まで辿ってエラーメッセージを有用なものにする.
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame *frame = &vm.frames[i];
    ObjFunction *function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    fprintf(stderr, "[line %d] in ", // [minus]
            function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack();
}

static void defineNative(const char *name, NativeFn function) {
  push(OBJ_VAL(copyString(name, (int) strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

// グローバル変数vmを初期化し, 実行の準備を行う
void initVM() {
  resetStack();
  vm.objects = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;

  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;

  initTable(&vm.globals);
  initTable(&vm.strings);

  vm.initString = NULL;
  vm.initString = copyString("init", 4);

  defineNative("clock", clockNative);
}

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  vm.initString = NULL;
  freeObjects();
}

// push はグローバル変数vmのスタックに引数の値をpushし, スタックポインタを一つ進める.
void push(Value value) {
  *vm.stackTop = value;
  vm.stackTop++;
}

// pop はグローバル変数vmのスタックポインタを一つ戻し, そこが指す値(Value)を返す.
Value pop() {
  // popしたスタックポインタが指している値を上書きすればスタック上の値を破棄したことと同じとみなせる
  vm.stackTop--;
  return *vm.stackTop;
}

// peek はlox仮想マシンのスタックを先頭から引数で指定された箇所の値を返す
static Value peek(int distance) {
  // 先頭のスタックポインタは常に一つ先の空の領域を指しているので, -1 して先頭の値からカウントさせる.
  return vm.stackTop[-1 - distance];
}

// クロージャ(関数)へのポインタと引数の数が渡される
static bool call(ObjClosure *closure, int argCount) {
  if (argCount != closure->function->arity) { // 引数の数チェック
    runtimeError("Expected %d arguments but got %d.",
                 closure->function->arity, argCount);
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame *frame = &vm.frames[vm.frameCount++];
  // 呼び出されたクロージャ(関数)オブジェクトでスタックトップの CallFrame を更新する
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  // スタックトップから (引数の数 + 1(関数オブジェクトの分)) したアドレスを CallFrame の先頭に設定する.
  // そうすると CallFrame の先頭は関数オブジェクトが slots[0] で参照できる.
  // よって引数は slots[1] から始まる.
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

// callValue は値に対して`()`演算子が呼ばれたときの処理を行う.
static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {
        ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
        vm.stackTop[-argCount - 1] = bound->receiver;
        return call(bound->method, argCount);
      }
      case OBJ_CLASS: {
        ObjClass *klass = AS_CLASS(callee);
        vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
        Value initializer;
        if (tableGet(&klass->methods, vm.initString,
                     &initializer)) {
          return call(AS_CLOSURE(initializer), argCount);
        } else if (argCount != 0) {
          runtimeError("Expected 0 arguments but got %d.",
                       argCount);
          return false;
        }
        return true;
      }
      // クロージャ対応のためすべての関数はOBJ_CLOSUREにラップされる
      case OBJ_CLOSURE:
        return call(AS_CLOSURE(callee), argCount);
      // Cネイティブ実装関数の処理
      case OBJ_NATIVE: {
        NativeFn native = AS_NATIVE(callee);
        // C言語実装なのでスタックなどを経由せず直接実行する
        Value result = native(argCount, vm.stackTop - argCount);
        // ネイティブ関数のために積まれたスタックを破棄
        vm.stackTop -= argCount + 1;
        push(result); // 結果をPUSH
        return true;
      }
      default:
        break; // Non-callable object type.
    }
  }
  runtimeError("Can only call functions and classes.");
  return false;
}

static bool invokeFromClass(ObjClass *klass, ObjString *name,
                            int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }
  return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString *name, int argCount) {
  Value receiver = peek(argCount);

  if (!IS_INSTANCE(receiver)) {
    runtimeError("Only instances have methods.");
    return false;
  }

  ObjInstance *instance = AS_INSTANCE(receiver);

  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    vm.stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }

  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass *klass, ObjString *name) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }

  ObjBoundMethod *bound = newBoundMethod(peek(0),
                                         AS_CLOSURE(method));
  pop();
  push(OBJ_VAL(bound));
  return true;
}

// captureUpvalue はOP_CLOSUREの初期化中に呼ばれる関数で,
// isLocal=trueなクロージャ変数を取得する処理.
// - 任意のローカル変数に対してObjUpvalueは一つしか存在しないようにしている.
static ObjUpvalue *captureUpvalue(Value *local) {
  ObjUpvalue *prevUpvalue = NULL;
  ObjUpvalue *upvalue = vm.openUpvalues;
  // ローカル変数をcloseするときは,リスト内の既存の値がないかをVM管理の連結リストから探す
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) {
    // 同じ変数をキャプチャしているupvalueがあるので, それを返す
    return upvalue;
  }

  // 新しいupValueオブジェクトを生成.
  ObjUpvalue *createdUpvalue = newUpvalue(local);

  // 新しいupvalueを連結リストに挿入する
  createdUpvalue->next = upvalue;

  if (prevUpvalue == NULL) {
    // リスト内のupvalueすべてが探しているslotの上のlocalsか, もしくはリストが空.
    // 先頭に追加.
    vm.openUpvalues = createdUpvalue;
  } else {
    // 現在closeしているslotを過ぎたので, そのslotに対応するupvalueはない.
    // 挿入
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

// closeUpvaluesはクロージャにキャプチャされた変数をヒープに退避させる.
// see: https://www.craftinginterpreters.com/image/closures/closing.png
static void closeUpvalues(Value *last) {
  // 破棄される変数より大きいアドレスはcloseの対象.
  while (vm.openUpvalues != NULL &&
         vm.openUpvalues->location >= last) {
    ObjUpvalue *upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location; // upvalueが現在指している値をデリファレンスして取得
    upvalue->location = &upvalue->closed; // 値の参照先を自身のclosedフィールドのアドレスに更新する.
    vm.openUpvalues = upvalue->next; // close されたので openUpvalues から除外
  }
}

static void defineMethod(ObjString *name) {
  Value method = peek(0);
  ObjClass *klass = AS_CLASS(peek(1));
  tableSet(&klass->methods, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
/* Strings concatenate < Garbage Collection concatenate-peek
  ObjString* b = AS_STRING(pop());
  ObjString* a = AS_STRING(pop());
*/
  ObjString *b = AS_STRING(peek(0));
  ObjString *a = AS_STRING(peek(1));

  int length = a->length + b->length;
  char *chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString *result = takeString(chars, length);
  pop();
  pop();
  push(OBJ_VAL(result));
}

// run は生成した lox バイトコードを実行する.
static InterpretResult run() {
  CallFrame *frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)

#define READ_SHORT() \
    (frame->ip += 2, \
    (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])

#define READ_STRING() AS_STRING(READ_CONSTANT())
/* A Virtual Machine binary-op < Types of Values binary-op
#define BINARY_OP(op) \
    do { \
      double b = pop(); \
      double a = pop(); \
      push(a op b); \
    } while (false)
*/
#define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        runtimeError("Operands must be numbers."); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(pop()); \
      push(valueType(a op b)); \
    } while (false)

  for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
      printf("[ ");
      printValue(*slot);
      printf(" ]");
    }
    printf("\n");
    disassembleInstruction(&frame->closure->function->chunk,
        (int)(frame->ip - frame->closure->function->chunk.code));
#endif

    uint8_t instruction;
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT: {
        Value constant = READ_CONSTANT();
/* A Virtual Machine op-constant < A Virtual Machine push-constant
        printValue(constant);
        printf("\n");
*/
        push(constant);
        break;
      }
      case OP_NIL:
        push(NIL_VAL);
        break;
      case OP_TRUE:
        push(BOOL_VAL(true));
        break;
      case OP_FALSE:
        push(BOOL_VAL(false));
        break;
      case OP_POP:
        pop();
        break;
      case OP_GET_LOCAL: {
        // ローカル変数のロード

        // ローカル変数が存在するスタックidxを1byteオペランドで取る
        uint8_t slot = READ_BYTE();
        // 現在CallFrameのslots先頭を経由して相対的にアクセスしてPUSHする.
        push(frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL: {
        // ローカル変数への代入
        uint8_t slot = READ_BYTE();
        // スタックの先頭から代入される値を取り出し, ローカル変数に対応するスタック・スロットに保存する.
        // スタックから値をポップしないことに注意.
        // 代入は式であり, すべての式は値を返す. よって代入式は代入された値を返すので, VMはスタックに値を残す.
        frame->slots[slot] = peek(0); // GET_OP_LOCAL 同様 CallFrame の slots 経由でセットする.
        break;
      }
      case OP_GET_GLOBAL: {
        ObjString *name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, name, &value)) {
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }
        push(value);
        break;
      }
      case OP_DEFINE_GLOBAL: {
        ObjString *name = READ_STRING();
        tableSet(&vm.globals, name, peek(0));
        pop();
        break;
      }
      case OP_SET_GLOBAL: {
        ObjString *name = READ_STRING();
        if (tableSet(&vm.globals, name, peek(0))) {
          tableDelete(&vm.globals, name); // [delete]
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_GET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        push(*frame->closure->upvalues[slot]->location);
        break;
      }
      case OP_SET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peek(0);
        break;
      }
      case OP_GET_PROPERTY: {
        if (!IS_INSTANCE(peek(0))) {
          runtimeError("Only instances have properties.");
          return INTERPRET_RUNTIME_ERROR;
        }

        ObjInstance *instance = AS_INSTANCE(peek(0));
        ObjString *name = READ_STRING();

        Value value;
        if (tableGet(&instance->fields, name, &value)) {
          pop(); // Instance.
          push(value);
          break;
        }

/* Classes and Instances get-undefined < Methods and Initializers get-method
        runtimeError("Undefined property '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
*/
        if (!bindMethod(instance->klass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_SET_PROPERTY: {
        if (!IS_INSTANCE(peek(1))) {
          runtimeError("Only instances have fields.");
          return INTERPRET_RUNTIME_ERROR;
        }

        ObjInstance *instance = AS_INSTANCE(peek(1));
        tableSet(&instance->fields, READ_STRING(), peek(0));
        Value value = pop();
        pop();
        push(value);
        break;
      }
      case OP_GET_SUPER: {
        ObjString *name = READ_STRING();
        ObjClass *superclass = AS_CLASS(pop());

        if (!bindMethod(superclass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_EQUAL: {
        Value b = pop();
        Value a = pop();
        push(BOOL_VAL(valuesEqual(a, b)));
        break;
      }
      case OP_GREATER:
        BINARY_OP(BOOL_VAL, >);
        break;
      case OP_LESS:
        BINARY_OP(BOOL_VAL, <);
        break;
/* A Virtual Machine op-binary < Types of Values op-arithmetic
      case OP_ADD:      BINARY_OP(+); break;
      case OP_SUBTRACT: BINARY_OP(-); break;
      case OP_MULTIPLY: BINARY_OP(*); break;
      case OP_DIVIDE:   BINARY_OP(/); break;
*/
/* A Virtual Machine op-negate < Types of Values op-negate
      case OP_NEGATE:   push(-pop()); break;
*/
/* Types of Values op-arithmetic < Strings add-strings
      case OP_ADD:      BINARY_OP(NUMBER_VAL, +); break;
*/
      case OP_ADD: {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
          concatenate();
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_NUMBER(pop());
          double a = AS_NUMBER(pop());
          push(NUMBER_VAL(a + b));
        } else {
          runtimeError(
              "Operands must be two numbers or two strings.");
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_SUBTRACT:
        BINARY_OP(NUMBER_VAL, -);
        break;
      case OP_MULTIPLY:
        BINARY_OP(NUMBER_VAL, *);
        break;
      case OP_DIVIDE:
        BINARY_OP(NUMBER_VAL, /);
        break;
      case OP_NOT:
        push(BOOL_VAL(isFalsey(pop())));
        break;
      case OP_NEGATE:
        if (!IS_NUMBER(peek(0))) {
          runtimeError("Operand must be a number.");
          return INTERPRET_RUNTIME_ERROR;
        }
        push(NUMBER_VAL(-AS_NUMBER(pop())));
        break;
      case OP_PRINT: {
        printValue(pop());
        printf("\n");
        break;
      }
      case OP_JUMP: {
        uint16_t offset = READ_SHORT();
/* Jumping Back and Forth op-jump < Calls and Functions jump
        vm.ip += offset;
*/
        frame->ip += offset;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0))) frame->ip += offset;
        break;
      }
      case OP_LOOP: {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        break;
      }
      // 関数の呼び出し命令.
      case OP_CALL: {
        int argCount = READ_BYTE();
        // VMのスタックの先頭に引数が積まれているので argCount の数だけ peek した箇所に関数が収められている.
        // その関数を callValue にわたす.
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        // 関数呼び出しに成功した場合VMのスタックに新しいCallFrameが積まれている.
        // それを現在実行している frame として更新する.
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_INVOKE: {
        ObjString *method = READ_STRING();
        int argCount = READ_BYTE();
        if (!invoke(method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_SUPER_INVOKE: {
        ObjString *method = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass *superclass = AS_CLASS(pop());
        if (!invokeFromClass(superclass, method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_CLOSURE: {
        ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure *closure = newClosure(function);
        push(OBJ_VAL(closure));
        // upvalueCount のぶんだけオペランドバイトコードを読み込む.
        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t index = READ_BYTE();
          if (isLocal) {
            // isLocal=true ならば「現在実行中のCallFrame」で宣言された関数がそのCallFrameで宣言された変数をキャプチャしているので,
            // frame->slots+index に位置にある変数をcaptureUpvalueでキャプチャする.
            closure->upvalues[i] =
                captureUpvalue(frame->slots + index);
          } else {
            // isLocal=falseは更に外部のスコープにある変数キャプチャなのでupvaluesのindexから参照チェーンを取得しておく
            closure->upvalues[i] = frame->closure->upvalues[index];
          }
        }
        break;
      }
      // キャプチャされた変数をヒープに退避させる命令.
      // コンパイラはブロックの終端に達するたび(関数定義除く)そのブロック内のすべてのローカル変数を破棄しクローズされた各ローカル変数に対して,
      // OP_CLOSE_UPVALUE を出力しなければならない.
      case OP_CLOSE_UPVALUE:
        closeUpvalues(vm.stackTop - 1);
        pop();
        break;
      // 関数からの復帰命令
      case OP_RETURN: {
        Value result = pop(); // 関数の実行結果を取得
        closeUpvalues(frame->slots); // 関数内部で定義された変数(引数含む)も正しくCLOSEされなければならない(入れ子関数定義でクロージャにキャプチャされる可能性がある).
        // CallFrame の破棄
        vm.frameCount--;
        if (vm.frameCount == 0) {
          // トップレベルのCallFrameの終了 = プログラム全体の終了
          pop();
          return INTERPRET_OK;
        }

        // 呼び終わった関数のCallFrame先頭をスタックトップに更新 = CallFrame が積んでいた値を破棄する.
        vm.stackTop = frame->slots;
        push(result); // 関数の結果を先頭に積む
        // 現在実行中の CallFrame を更新する
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      case OP_CLASS:
        push(OBJ_VAL(newClass(READ_STRING())));
        break;
      case OP_INHERIT: {
        Value superclass = peek(1);
        if (!IS_CLASS(superclass)) {
          runtimeError("Superclass must be a class.");
          return INTERPRET_RUNTIME_ERROR;
        }

        ObjClass *subclass = AS_CLASS(peek(0));
        tableAddAll(&AS_CLASS(superclass)->methods,
                    &subclass->methods);
        pop(); // Subclass.
        break;
      }
      case OP_METHOD:
        defineMethod(READ_STRING());
        break;
    }
  }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

void hack(bool b) {
  // Hack to avoid unused function error. run() is not used in the
  // scanning chapter.
  run();
  if (b) hack(false);
}

// interpret は入力された lox 言語を実行する入り口.
InterpretResult interpret(const char *source) {
  ObjFunction *function = compile(source);
  if (function == NULL) return INTERPRET_COMPILE_ERROR;

  // スクリプトをコンパイルするとき, まだRAWの関数オブジェクトを返す
  // NOTE: 関数オブジェクトをわざわざ PUSH/POP しているのはヒープに割り当てられたオブジェクトをGCに認識させるために必要な処理である.
  push(OBJ_VAL(function));
/*
  CallFrame* frame = &vm.frames[vm.frameCount++]; // 先頭の CallFrame[0] を最初の関数に設定.
  frame->function = function;                     // トップレベル関数(暗黙的main)を参照させる.
  frame->ip = function->chunk.code;               // 命令ポインタを関数チャンクの先頭に設定.
  frame->slots = vm.stack;                        // Stackの先頭アドレスを関数のslotsに設定.
*/

  ObjClosure *closure = newClosure(function); // VMが実行する前にRAW関数オブジェクトをクロージャOBJでラップする
  pop(); // RAW関数オブジェクトはもういらないのでPOP
  push(OBJ_VAL(closure)); // クロージャオブジェクトをPUSH
  call(closure, 0); // 暗黙的に定義された _main を呼び出す.

/* Compiling Expressions interpret-chunk < Calls and Functions end-interpret
  InterpretResult result = run();

  freeChunk(&chunk);
  return result;
*/
  return run();
}
