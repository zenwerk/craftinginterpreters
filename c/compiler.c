#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "scanner.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

// Parser はパーサーを表す.
typedef struct {
  Token current;  // 現在読み込んでいる字句
  Token previous; // 一つ前の字句
  bool hadError;
  bool panicMode; // エラーの雪崩を回避するためのモード
} Parser;

// Precedence は演算子の優先順位を表す.
// 優先順位が低いものから高い順に並んでいる.
typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  // =          代入は最も優先順位が低い -> canAssign フラグの必要性が生じる
  PREC_OR,          // or         論理OR
  PREC_AND,         // and        論理AND
  PREC_EQUALITY,    // == !=      論理比較
  PREC_COMPARISON,  // < > <= >=  数値比較
  PREC_TERM,        // + -        二項演算+-
  PREC_FACTOR,      // * /        二項演算*/
  PREC_UNARY,       // ! -        単項演算子
  PREC_CALL,        // . ()       グルーピング
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

// ParseRule は構文解析のルールを表す
// あるトークンが与えられたときに...
typedef struct {
  ParseFn prefix;  // その型のトークンで始まる接頭辞式をコンパイルする関数
  ParseFn infix;   // 左オペランドにその型のトークンが続くinfix式をコンパイルする関数
  Precedence precedence; // そのトークンを演算子として使用するinfix式の優先順位
} ParseRule;

// ローカル変数を表す構造体
typedef struct {
  Token name; // ローカル変数名
  int depth;  // ローカル変数が宣言されたブロックのスコープ深度
  bool isCaptured;
} Local;

typedef struct {
  uint8_t index;
  bool isLocal;
} Upvalue;

typedef enum {
  TYPE_FUNCTION,
  TYPE_INITIALIZER,
  TYPE_METHOD,
  TYPE_SCRIPT
} FunctionType;

// コンパイラを表す構造体
typedef struct Compiler {
  struct Compiler *enclosing;
  ObjFunction *function;
  FunctionType type;

  // ローカル変数の指定に使えるオペランドが1byteであるため,
  // 一つのブロックに登録できるローカル変数は UINT8_COUNT 個まで, という制限が生まれる.
  Local locals[UINT8_COUNT]; // どのスタックスロットがどのローカル変数やテンポラリに関連付けられているかを追跡する
  int localCount; // スコープ内にローカル変数が何個があるか, つまりlocals配列がいくつ使用中であるかを示す変数.
  Upvalue upvalues[UINT8_COUNT];
  int scopeDepth; // スコープの深さ, つまり現在コンパイル中のコードがいくつ {} で囲まれているかを示す.
} Compiler;

typedef struct ClassCompiler {
  struct ClassCompiler *enclosing;
  bool hasSuperclass;
} ClassCompiler;

Parser parser; // パーサーはグローバル変数で管理.
Compiler *current = NULL; // current は現在有効なCompiler構造体を示す.
ClassCompiler *currentClass = NULL;

/* Compiling Expressions compiling-chunk < Calls and Functions current-chunk
Chunk* compilingChunk;

static Chunk* currentChunk() {
  return compilingChunk;
}
*/

static Chunk *currentChunk() {
  return &current->function->chunk;
}

static void errorAt(Token *token, const char *message) {
  // すでに構文エラーが発生しているなら後続のエラーは無視する
  if (parser.panicMode) return;
  parser.panicMode = true; // 構文エラー発生フラグをON
  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // Nothing.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

static void error(const char *message) {
  errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char *message) {
  errorAt(&parser.current, message);
}

// (パーサー) advance は字句を一つ読み込み, パーサーに現在解析中の字句として設定する
static void advance() {
  // 既存の字句を一つ前の字句に移動する
  parser.previous = parser.current;

  for (;;) {
    // スキャナを進め解析中の字句を更新する
    parser.current = scanToken();
    // エラーでなければ抜ける
    if (parser.current.type != TOKEN_ERROR)
      break;

    errorAtCurrent(parser.current.start);
  }
}

// (パーサー) consume は現在の字句が指定されたTokenTypeか調べ, そうであれば字句を一つ読み込む.
// 違う場合はエラーメッセージを表示しパースエラー状態となる.
static void consume(TokenType type, const char *message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

// check はパーサーの現在の字句が指定された種類かチェックし真偽値を返す.
static bool check(TokenType type) {
  return parser.current.type == type;
}

// match はパーサーの現在の字句が指定された種類か確認し真偽値を返す.
// 指定された字句だった場合, パーサーは新たな字句を一つ読み込む.
static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

static void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

static int emitJump(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}

static void emitReturn() {
/* Calls and Functions return-nil < Methods and Initializers return-this
  emitByte(OP_NIL);
*/
  if (current->type == TYPE_INITIALIZER) {
    emitBytes(OP_GET_LOCAL, 0);
  } else {
    emitByte(OP_NIL);
  }

  emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX) {
    error("Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t)constant;
}

static void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}

static void patchJump(int offset) {
  // -2 to adjust for the bytecode for the jump offset itself.
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }

  currentChunk()->code[offset] = (jump >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jump & 0xff;
}

/* Local Variables init-compiler < Calls and Functions init-compiler
static void initCompiler(Compiler* compiler) {
*/
static void initCompiler(Compiler *compiler, FunctionType type) {
  compiler->enclosing = current;
  compiler->function = NULL;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->function = newFunction();
  current = compiler;
  if (type != TYPE_SCRIPT) {
    current->function->name = copyString(parser.previous.start,
                                         parser.previous.length);
  }

  Local *local = &current->locals[current->localCount++];
  local->depth = 0;
  local->isCaptured = false;
/* Calls and Functions init-function-slot < Methods and Initializers slot-zero
  local->name.start = "";
  local->name.length = 0;
*/
  if (type != TYPE_FUNCTION) {
    local->name.start = "this";
    local->name.length = 4;
  } else {
    local->name.start = "";
    local->name.length = 0;
  }
}

static ObjFunction *endCompiler() {
  emitReturn();
  ObjFunction *function = current->function;

#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
/* Compiling Expressions dump-chunk < Calls and Functions disassemble-end
    disassembleChunk(currentChunk(), "code");
*/
    disassembleChunk(currentChunk(), function->name != NULL
        ? function->name->chars : "<script>");
  }
#endif

  current = current->enclosing;
  return function;
}

// beginScope は現在のコンパイラ構造体のスコープ深度をインクリメントする
static void beginScope() {
  current->scopeDepth++;
}

// endScope はスコープを抜けるときの処理を行う.
static void endScope() {
  current->scopeDepth--; // 現在のスコープ深度をデクリメントする

  // ローカル変数が存在する && 現在のスコープ深度よりローカル変数の深度が大きい場合(抜けたブロック内で定義されたローカル変数か？)
  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth > current->scopeDepth) {
    if (current->locals[current->localCount - 1].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      // ローカル変数をスタックからPOPして破棄する.
      emitByte(OP_POP);
    }
    // 変数を破棄したのでカウント数をデクリメントする.
    current->localCount--;
  }
}

static void expression();

static void statement();

static void declaration();

static ParseRule *getRule(TokenType type);

static void parsePrecedence(Precedence precedence);

static uint8_t identifierConstant(Token *name) {
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token *a, Token *b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name) {
  // コンパイラ構造体に登録されているローカル変数をなめる.
  // 末尾からループすることでスコープ外の同名の変数をシャドーイングできる.
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local *local = &compiler->locals[i];
    // 指定された名前の変数か？
    if (identifiersEqual(name, &local->name)) {
      if (local->depth == -1) {
        // 初期化中に自分自身を自己参照することはできないというエラー
        // `{ var a = 1; { var a = a * 3;} }` みたいな代入式.
        error("can't read local variable in its own initializer.");
      }
      return i;
    }
  }

  // 見つからなかった
  return -1;
}

static int addUpvalue(Compiler *compiler, uint8_t index,
                      bool isLocal) {
  int upvalueCount = compiler->function->upvalueCount;

  for (int i = 0; i < upvalueCount; i++) {
    Upvalue *upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  if (upvalueCount == UINT8_COUNT) {
    error("Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler *compiler, Token *name) {
  if (compiler->enclosing == NULL) return -1;

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t) local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t) upvalue, false);
  }

  return -1;
}

// addLocal は現在のコンパイラ構造体にローカル変数`name`を追加する
static void addLocal(Token name) {
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function.");
    return;
  }

  Local *local = &current->locals[current->localCount++]; // ローカル変数の保存アドレスを取得する
  // 取得したアドレスにローカル変数を保存する
  local->name = name;
  local->depth = -1; // depth = -1 は変数が未初期化の状態であることを示す.
  local->isCaptured = false;
}

// declareVariable は変数を宣言する.
// 宣言とは, コンパイラが変数の存在を記録することである.
static void declareVariable() {
  // グローバル変数の宣言なら何もしない
  if (current->scopeDepth == 0)
    return;

  // 変数名を取得する
  Token *name = &parser.previous;
  // 現在のローカル変数をすべてなめる
  for (int i = current->localCount - 1; i >= 0; i--) {
    Local *local = &current->locals[i];
    // ローカル変数のスコープ深度 != -1 かつ ローカル変数のスコープはコンパイラの現在のスコープの外側なら,
    // ループを抜ける. (無駄なローカル変数の重複確認を行わないということ)
    if (local->depth != -1 && local->depth < current->scopeDepth) {
      break; // [negative]
    }

    // 同じ変数名が宣言されていないか確認する
    if (identifiersEqual(name, &local->name)) {
      error("Already a variable with this name in this scope.");
    }
  }

  addLocal(*name);
}

// parseVariable は変数名を解析する
static uint8_t parseVariable(const char *errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);

  // 変数の宣言 -> コンパイラ構造体に変数を記録.
  declareVariable();
  // ブロックスコープ内の変数宣言ならここで return
  if (current->scopeDepth > 0)
    return 0;

  return identifierConstant(&parser.previous);
}

static void markInitialized() {
  // コンパイラがトップレベルを解析中なら, それはグローバル変数なので何もしない
  // lox ではグローバル変数はlate binding なので実行時に初期化済みであれば問題ない仕様のため.
  if (current->scopeDepth == 0)
    return;

  // 変数が定義されたスコープ(その変数を所有するスコープ)の深度を記録する
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}

// defineVariable は変数を定義する.
// 変数定義のための特別なバイトコードはemitしない.
// なぜならスタックの先頭にすでにローカル変数となる値がPUSHされているからである.
// 引数globalはグローバル変数のときだけ使用される.
static void defineVariable(uint8_t global) {
  // 非トップレベルの変数(ローカル変数)の場合
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  // グローバル変数の場合
  emitBytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList() {
  uint8_t argCount = 0;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      expression();
      if (argCount == 255) {
        error("Can't have more than 255 arguments.");
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  return argCount;
}

static void and_(bool canAssign) {
  int endJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJump(endJump);
}

// 二項演算子を解析する関数.
// この関数が呼ばれるとき, 例えば `1+2` のとき, `1+` までパーサーは字句を読み込んでいる状態.
// つまり,左オペランドはすでに解析済み, 次に `+` を読み込んで現在解析しているものが二項演算子であるとわかったタイミングである.
// よって残る処理は右オペランドを解析し最後に演算子に基づくバイトコードをPUSHすることである.
static void binary(bool canAssign) {
  TokenType operatorType = parser.previous.type;
  ParseRule *rule = getRule(operatorType);
  // 例えば `2*3+4` が与えられたとき `(2*3)+4` と解析されるべきなので
  // 現在の演算子より一つ高い優先順位で解析を行わないといけない => `rule->precedence + 1` が必要
  // 同じ優先順位でもよいのでは? となるが`1*2*3*4`なら`((1*2)*3)*4`というふうに解析したいのである(ただし左結合の場合).
  // NOTE: 右結合(例えば代入式`a = (b = (c = d))`)の場合は「同じ優先順位」でparsePrecedenceを呼ぶ.
  parsePrecedence((Precedence) (rule->precedence + 1));

  // 最後に二項演算子に基づくバイトコードをPUSHする.
  switch (operatorType) {
    case TOKEN_BANG_EQUAL:
      emitBytes(OP_EQUAL, OP_NOT);
      break;
    case TOKEN_EQUAL_EQUAL:
      emitByte(OP_EQUAL);
      break;
    case TOKEN_GREATER:
      emitByte(OP_GREATER);
      break;
    case TOKEN_GREATER_EQUAL:
      emitBytes(OP_LESS, OP_NOT);
      break;
    case TOKEN_LESS:
      emitByte(OP_LESS);
      break;
    case TOKEN_LESS_EQUAL:
      emitBytes(OP_GREATER, OP_NOT);
      break;
    case TOKEN_PLUS:
      emitByte(OP_ADD);
      break;
    case TOKEN_MINUS:
      emitByte(OP_SUBTRACT);
      break;
    case TOKEN_STAR:
      emitByte(OP_MULTIPLY);
      break;
    case TOKEN_SLASH:
      emitByte(OP_DIVIDE);
      break;
    default:
      return; // Unreachable.
  }
}

static void call(bool canAssign) {
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static void dot(bool canAssign) {
  consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
  uint8_t name = identifierConstant(&parser.previous);

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(OP_SET_PROPERTY, name);
  } else if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCount = argumentList();
    emitBytes(OP_INVOKE, name);
    emitByte(argCount);
  } else {
    emitBytes(OP_GET_PROPERTY, name);
  }
}

static void literal(bool canAssign) {
  switch (parser.previous.type) {
    case TOKEN_FALSE:
      emitByte(OP_FALSE);
      break;
    case TOKEN_NIL:
      emitByte(OP_NIL);
      break;
    case TOKEN_TRUE:
      emitByte(OP_TRUE);
      break;
    default:
      return; // Unreachable.
  }
}

static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void or_(bool canAssign) {
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP);

  patchJump(elseJump);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJump(endJump);
}

static void string(bool canAssign) {
  emitConstant(OBJ_VAL(copyString(parser.previous.start + 1,
                                  parser.previous.length - 2)));
}

// namedVariable は変数の値の設定・取得を行う
static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  // arg はバイトコードにemitされるオペランド.
  // 値は指定されたコンパイラ構造体の locals配列のインデックス.
  int arg = resolveLocal(current, &name);
  // -1 はローカル変数には登録されていない名前である = グローバル変数である
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  // 代入式が許可されているコンテキストがあり, IDトークンの後に `=` が続くなら代入処理(setter).
  // なければ値参照(getter).
  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, (uint8_t) arg);
  } else {
    emitBytes(getOp, (uint8_t) arg);
  }
}

// 変数名を解析する接頭辞パーサー.
static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

static Token syntheticToken(const char *text) {
  Token token;
  token.start = text;
  token.length = (int) strlen(text);
  return token;
}

static void super_(bool canAssign) {
  if (currentClass == NULL) {
    error("Can't use 'super' outside of a class.");
  } else if (!currentClass->hasSuperclass) {
    error("Can't use 'super' in a class with no superclass.");
  }

  consume(TOKEN_DOT, "Expect '.' after 'super'.");
  consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
  uint8_t name = identifierConstant(&parser.previous);

  namedVariable(syntheticToken("this"), false);
/* Superclasses super-get < Superclasses super-invoke
  namedVariable(syntheticToken("super"), false);
  emitBytes(OP_GET_SUPER, name);
*/
  if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCount = argumentList();
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_SUPER_INVOKE, name);
    emitByte(argCount);
  } else {
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_GET_SUPER, name);
  }
}

static void this_(bool canAssign) {
  if (currentClass == NULL) {
    error("Can't use 'this' outside of a class.");
    return;
  }

  variable(false);
} // [this]

static void unary(bool canAssign) {
  // 単項演算子を取得する
  TokenType operatorType = parser.previous.type;

  // 単項演算子に続く式(ただし単項演算子より優先順位が高いもの)を先にパースする.
  // `-a.b+c` という式があるとき, 優先順位が未指定だと`-(a.b+c)`と解釈されてしまう. 正しくは`-(a.b)+c`
  parsePrecedence(PREC_UNARY);

  // Emit the operator instruction.
  // 最後に単項演算子のバイトコードをPUSH
  switch (operatorType) {
    case TOKEN_BANG:
      emitByte(OP_NOT);
      break;
    case TOKEN_MINUS:
      emitByte(OP_NEGATE);
      break;
    default:
      return; // Unreachable.
  }
}

// あるトークンが与えられらたときに, どのように解析するかを示すテーブル.
// {NULL, NULL, PREC_NONE} なエントリは, そのトークンは式では使用されない字句であることを示している.
ParseRule rules[] = {
/* Compiling Expressions rules < Calls and Functions infix-left-paren */
    [TOKEN_LEFT_PAREN]    = {grouping, call, PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL, NULL, PREC_NONE}, // [big]
    [TOKEN_RIGHT_BRACE]   = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA]         = {NULL, NULL, PREC_NONE},
/* Compiling Expressions rules < Classes and Instances table-dot */
    [TOKEN_DOT]           = {NULL, dot, PREC_CALL},
    [TOKEN_MINUS]         = {unary, binary, PREC_TERM}, // `-`
    [TOKEN_PLUS]          = {NULL, binary, PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL, NULL, PREC_NONE},
    [TOKEN_SLASH]         = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR]          = {NULL, binary, PREC_FACTOR},
/* Compiling Expressions rules < Types of Values table-not */
    [TOKEN_BANG]          = {unary, NULL, PREC_NONE},
/* Compiling Expressions rules < Types of Values table-equal */
    [TOKEN_BANG_EQUAL]    = {NULL, binary, PREC_EQUALITY},
    [TOKEN_EQUAL]         = {NULL, NULL, PREC_NONE},
/* Compiling Expressions rules < Types of Values table-comparisons */
    [TOKEN_EQUAL_EQUAL]   = {NULL, binary, PREC_EQUALITY},
    [TOKEN_GREATER]       = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL, binary, PREC_COMPARISON},
/* Compiling Expressions rules < Global Variables table-identifier */
    [TOKEN_IDENTIFIER]    = {variable, NULL, PREC_NONE},
/* Compiling Expressions rules < Strings table-string */
    [TOKEN_STRING]        = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER]        = {number, NULL, PREC_NONE}, // 数値リテラル
/* Compiling Expressions rules < Jumping Back and Forth table-and */
    [TOKEN_AND]           = {NULL, and_, PREC_AND},
    [TOKEN_CLASS]         = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE]          = {NULL, NULL, PREC_NONE},
/* Compiling Expressions rules < Types of Values table-false */
    [TOKEN_FALSE]         = {literal, NULL, PREC_NONE},
    [TOKEN_FOR]           = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN]           = {NULL, NULL, PREC_NONE},
    [TOKEN_IF]            = {NULL, NULL, PREC_NONE},
/* Compiling Expressions rules < Types of Values table-nil */
    [TOKEN_NIL]           = {literal, NULL, PREC_NONE},
/* Compiling Expressions rules < Jumping Back and Forth table-or */
    [TOKEN_OR]            = {NULL, or_, PREC_OR},
    [TOKEN_PRINT]         = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN]        = {NULL, NULL, PREC_NONE},
/* Compiling Expressions rules < Superclasses table-super */
    [TOKEN_SUPER]         = {super_, NULL, PREC_NONE},
/* Compiling Expressions rules < Methods and Initializers table-this */
    [TOKEN_THIS]          = {this_, NULL, PREC_NONE},
/* Compiling Expressions rules < Types of Values table-true */
    [TOKEN_TRUE]          = {literal, NULL, PREC_NONE},
    [TOKEN_VAR]           = {NULL, NULL, PREC_NONE},
    [TOKEN_WHILE]         = {NULL, NULL, PREC_NONE},
    [TOKEN_ERROR]         = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF]           = {NULL, NULL, PREC_NONE},
};

// parsePrecedence は指定された優先順位レベル以上の「式」(文じゃないよ)を解析する.
// 優先順位を明示することで `-a.b + c` を -(a.b + c) のように解釈してしまうことを防ぐ.
// NOTE: この関数は各パースルールの関数から再帰的に呼び出されるものだということを意識してコードを読もう.
static void parsePrecedence(Precedence precedence) {
  advance(); // 字句を一つ読み込み先に進める.
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  // 最初のトークンは定義上、常に何らかの接頭辞表現に属することになる. コードを左から右に読んでいくと, 最初にヒットするトークンは常にプレフィックス式に属している.
  if (prefixRule == NULL) {
    // 接頭辞ルールが存在しない字句から始まっているのは文法エラーな「式」が与えられたパターン.
    // 例えば `else` から始まる式など.
    error("Expect expression.");
    return;
  }

  // 演算子の優先順位が代入式より低い場合のみ,
  // `=`トークンを探して消費できることを示すフラグ.
  // 本文の `a * b = c + d` のパース例を読むこと.
  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign); // 接頭辞式の解析を行う

  // 後続のトークンに対応するinfixパースルールを探す.
  // もしあるなら, すでにコンパイルしたprefixルールはオペランドなのかもしれない.
  while (precedence <= getRule(parser.current.type)->precedence) { // 指定された優先順位以上のパースルールの間だけパースを続ける.
    advance(); // 字句を取得し
    ParseFn infixRule = getRule(parser.previous.type)->infix; // パース関数を取得し
    infixRule(canAssign); // パースを実行し, 次のループへ進み, さらにパースを進めるか判断する.
  }

  // 代入式が許可されたコンテキストにいながら `=` が消費されず残っているなら構文エラー.
  if (canAssign && match(TOKEN_EQUAL)) {
    error("Invalid assignment target.");
  }
}

static ParseRule *getRule(TokenType type) {
  return &rules[type];
}

// expression は式を解析する.
static void expression() {
  parsePrecedence(PREC_ASSIGNMENT);
}

static void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope(); // [no-end-scope]

  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current->function->arity++;
      if (current->function->arity > 255) {
        errorAtCurrent("Can't have more than 255 parameters.");
      }
      uint8_t constant = parseVariable("Expect parameter name.");
      defineVariable(constant);
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  block();

  ObjFunction *function = endCompiler();
/* Calls and Functions compile-function < Closures emit-closure
  emitBytes(OP_CONSTANT, makeConstant(OBJ_VAL(function)));
*/
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void method() {
  consume(TOKEN_IDENTIFIER, "Expect method name.");
  uint8_t constant = identifierConstant(&parser.previous);

/* Methods and Initializers method-body < Methods and Initializers method-type
  FunctionType type = TYPE_FUNCTION;
*/
  FunctionType type = TYPE_METHOD;
  if (parser.previous.length == 4 &&
      memcmp(parser.previous.start, "init", 4) == 0) {
    type = TYPE_INITIALIZER;
  }

  function(type);
  emitBytes(OP_METHOD, constant);
}

static void classDeclaration() {
  consume(TOKEN_IDENTIFIER, "Expect class name.");
  Token className = parser.previous;
  uint8_t nameConstant = identifierConstant(&parser.previous);
  declareVariable();

  emitBytes(OP_CLASS, nameConstant);
  defineVariable(nameConstant);

  ClassCompiler classCompiler;
  classCompiler.hasSuperclass = false;
  classCompiler.enclosing = currentClass;
  currentClass = &classCompiler;

  if (match(TOKEN_LESS)) {
    consume(TOKEN_IDENTIFIER, "Expect superclass name.");
    variable(false);

    if (identifiersEqual(&className, &parser.previous)) {
      error("A class can't inherit from itself.");
    }

    beginScope();
    addLocal(syntheticToken("super"));
    defineVariable(0);

    namedVariable(className, false);
    emitByte(OP_INHERIT);
    classCompiler.hasSuperclass = true;
  }

  namedVariable(className, false);
  consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    method();
  }
  consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
  emitByte(OP_POP);

  if (classCompiler.hasSuperclass) {
    endScope();
  }

  currentClass = currentClass->enclosing;
}

static void funDeclaration() {
  uint8_t global = parseVariable("Expect function name.");
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

// varDeclaration は変数宣言文を解析する
static void varDeclaration() {
  uint8_t global = parseVariable("Expect variable name.");

  // ここで emit されるバイトコード(の演算結果の値)がローカル変数を表すことになる
  if (match(TOKEN_EQUAL)) {
    // 初期化式のパース
    expression();
  } else {
    // 未初期化変数はすべて nil で初期化する
    emitByte(OP_NIL);
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  // 変数の初期化を行う
  defineVariable(global);
}

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void forStatement() {
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
/* Jumping Back and Forth for-statement < Jumping Back and Forth for-initializer
  consume(TOKEN_SEMICOLON, "Expect ';'.");
*/
  if (match(TOKEN_SEMICOLON)) {
    // No initializer.
  } else if (match(TOKEN_VAR)) {
    varDeclaration();
  } else {
    expressionStatement();
  }

  int loopStart = currentChunk()->count;
/* Jumping Back and Forth for-statement < Jumping Back and Forth for-exit
  consume(TOKEN_SEMICOLON, "Expect ';'.");
*/
  int exitJump = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    // Jump out of the loop if the condition is false.
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }

/* Jumping Back and Forth for-statement < Jumping Back and Forth for-increment
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");
*/
  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(loopStart);
    loopStart = incrementStart;
    patchJump(bodyJump);
  }

  statement();
  emitLoop(loopStart);

  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }

  endScope();
}

static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition."); // [paren]

  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  int elseJump = emitJump(OP_JUMP);

  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();
  patchJump(elseJump);
}

static void printStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  emitByte(OP_PRINT);
}

static void returnStatement() {
  if (current->type == TYPE_SCRIPT) {
    error("Can't return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON)) {
    emitReturn();
  } else {
    if (current->type == TYPE_INITIALIZER) {
      error("Can't return a value from an initializer.");
    }

    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
    emitByte(OP_RETURN);
  }
}

static void whileStatement() {
  int loopStart = currentChunk()->count;
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();
  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP);
}

static void synchronize() {
  parser.panicMode = false;

  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON) return;
    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_VAR:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
        return;

      default:; // Do nothing.
    }

    advance();
  }
}

// declaration は宣言をパースする.
// 宣言はソースコードのトップレベルやブロック内で許可された文法を処理する.
static void declaration() {
  if (match(TOKEN_CLASS)) {
    classDeclaration();
  } else if (match(TOKEN_FUN)) {
    funDeclaration();
  } else if (match(TOKEN_VAR)) {
    // 変数制限の解析
    varDeclaration();
  } else {
    // 宣言と文は明確に分ける
    statement();
  }

  if (parser.panicMode) synchronize();
}

// statement は文を解析する.
// 文はコントロールフロー内で許可された文法を処理する.
static void statement() {
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_FOR)) {
    forStatement();
  } else if (match(TOKEN_IF)) {
    ifStatement();
  } else if (match(TOKEN_RETURN)) {
    returnStatement();
  } else if (match(TOKEN_WHILE)) {
    whileStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

/* Scanning on Demand compiler-c < Compiling Expressions compile-signature
void compile(const char* source) {
*/
/* Compiling Expressions compile-signature < Calls and Functions compile-signature
bool compile(const char* source, Chunk* chunk) {
*/
ObjFunction *compile(const char *source) {
  initScanner(source);
/* Scanning on Demand dump-tokens < Compiling Expressions compile-chunk
  int line = -1;
  for (;;) {
    Token token = scanToken();
    if (token.line != line) {
      printf("%4d ", token.line);
      line = token.line;
    } else {
      printf("   | ");
    }
    printf("%2d '%.*s'\n", token.type, token.length, token.start); // [format]

    if (token.type == TOKEN_EOF) break;
  }
*/
  Compiler compiler;
/* Local Variables compiler < Calls and Functions call-init-compiler
  initCompiler(&compiler);
*/
  initCompiler(&compiler, TYPE_SCRIPT);
/* Compiling Expressions init-compile-chunk < Calls and Functions call-init-compiler
  compilingChunk = chunk;
*/

  parser.hadError = false;
  parser.panicMode = false;

  advance(); // 最初の字句を解析しパーサーに設定する.
/* Compiling Expressions compile-chunk < Global Variables compile
  expression();
  consume(TOKEN_EOF, "Expect end of expression.");
*/

  while (!match(TOKEN_EOF)) {
    declaration();
  }

/* Compiling Expressions finish-compile < Calls and Functions call-end-compiler
  endCompiler();
*/
/* Compiling Expressions return-had-error < Calls and Functions call-end-compiler
  return !parser.hadError;
*/
  ObjFunction *function = endCompiler();
  return parser.hadError ? NULL : function;
}

void markCompilerRoots() {
  Compiler *compiler = current;
  while (compiler != NULL) {
    markObject((Obj *) compiler->function);
    compiler = compiler->enclosing;
  }
}
