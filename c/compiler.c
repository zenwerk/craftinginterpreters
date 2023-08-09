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

// Upvalue はクロージャにキャプチャされた変数を表す
typedef struct {
  uint8_t index; // 閉包された変数のlocalsインデックスを追跡する. そうすることで、コンパイラは、囲んでいる関数内のどの変数をキャプチャする必要があるかを認識する.
  bool isLocal;
} Upvalue;

// コンパイルされる関数の種類を指定する
typedef enum {
  TYPE_FUNCTION, // 通常定義関数
  TYPE_INITIALIZER,
  TYPE_METHOD,
  TYPE_SCRIPT // トップレベルのコード
} FunctionType;

// コンパイラを表す構造体. 最終的にこの構造体は関数をコンパイルする毎に生成される.
// この構造体が管理するものは,
// - コンパイルしている関数
// - ローカル変数
// - スコープの深さ
typedef struct Compiler {
  struct Compiler *enclosing; // 連結リスト用 (Compiler構造体はコンパイルする関数毎に生成される)
  ObjFunction *function; // コンパイルする関数オブジェクトへの参照
  FunctionType type;

  // ローカル変数の指定に使えるオペランドが1byteであるため,
  // 一つのブロックに登録できるローカル変数は UINT8_COUNT 個まで, という制限が生まれる.
  Local locals[UINT8_COUNT];     // どのスタックスロットがどのローカル変数やテンポラリに関連付けられているかを追跡する.
  int localCount;                // スコープ内にローカル変数が何個があるか, つまりlocals配列がいくつ使用中であるかを示す変数.
  Upvalue upvalues[UINT8_COUNT]; // 関数のボディで解決されたクロージャ変数を追跡するための配列.
  int scopeDepth;                // スコープの深さ, つまり現在コンパイル中のコードがいくつ {} で囲まれているかを示す.
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

// currentChunk は __現在コンパイルしている__ 関数のChunkへの参照を返す
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

  // +2 は OP_LOOP命令自身のサイズを考慮したオフセット.
  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

// emitJump はジャンプ命令を出力する
static int emitJump(uint8_t instruction) {
  emitByte(instruction);
  // ジャンプ先のアドレスは 0xffff で仮置きしておく.
  // 後ほど patchJump 関数で実際のオペランドの値に置換する必要がある.
  emitByte(0xff);
  emitByte(0xff);
  // 現在のバイトコードの 2byte 前のアドレス値を返しておく.
  // 後からこのアドレスから 2byte 置換する.
  return currentChunk()->count - 2;
}

static void emitReturn() {
  if (current->type == TYPE_INITIALIZER) {
    emitBytes(OP_GET_LOCAL, 0);
  } else {
    // 明示的 return がない void な関数には NIL をPUSHする命令を仕込む.
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
  // jump先のアドレス.
  // -2 to adjust for the bytecode for the jump offset itself.
  // jumpオフセット自身の分を調整するため -2 する.
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }

  // 0xffff で仮置きしていたオペランドを実際のjump先に置換する
  currentChunk()->code[offset] = (jump >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jump & 0xff;
}

// initCompiler はコンパイラ構造体を初期化する.
static void initCompiler(Compiler *compiler, FunctionType type) {
  compiler->enclosing = current; // 現在使用されているCompiler構造体を退避
  compiler->function = NULL; // ガベージコレクション回避のためのパラノイア的操作.
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->function = newFunction(); // コンパイルするための新しい関数オブジェクトを確保.
  current = compiler; // 現在コンパイルしている関数(Compiler)を更新する
  // 通常の関数定義の場合はここでコンパイルする関数名を取得する
  if (type != TYPE_SCRIPT) {
    current->function->name = copyString(parser.previous.start,
                                         parser.previous.length);
  }

  // 以下の1行は locals[0] をVM内部で使用することを暗黙的に示している.
  Local *local = &current->locals[current->localCount++];
  local->depth = 0; // ネスト数 0 はトップレベルコードである.
  local->isCaptured = false;
  if (type != TYPE_FUNCTION) {
    local->name.start = "this";
    local->name.length = 4;
  } else {
    local->name.start = "";  // ユーザーはVMのスタックスペースを指定できないように空文字列を与える.
    local->name.length = 0;
  }
}

static ObjFunction *endCompiler() {
  emitReturn(); // RETURN命令を追加する
  ObjFunction *function = current->function; // 現在コンパイル中の関数オブジェクト参照を取得.

#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), function->name != NULL
        ? function->name->chars : "<script>");
  }
#endif

  current = current->enclosing; // 退避していたCompiler構造体を戻す

  return function; // 取得した関数を返す
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

// resolveLocal は指定された名前のローカル変数を探しlocals配列のidxを返す.
// 見つからない場合は -1 を返す.
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

// addUpvalue はクロージャで閉包された値(upvalue)を作成する.
// isLocalフラグは外側関数の変数をキャプチャしているのかどうかを表す.
static int addUpvalue(Compiler *compiler, uint8_t index,
                      bool isLocal) {
  int upvalueCount = compiler->function->upvalueCount;

  // すでに登録済みのクロージャ変数かどうか確認し, 登録済みならそのidxを返す
  for (int i = 0; i < upvalueCount; i++) {
    Upvalue *upvalue = &compiler->upvalues[i];
    // すでに登録済みの Upvalue と同じ値か比較する
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  if (upvalueCount == UINT8_COUNT) {
    error("Too many closure variables in function.");
    return 0;
  }

  // 新しい upValue を登録する
  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  return compiler->function->upvalueCount++;
}

// resolveUpvalue は外部のブロックで定義されている(かもしれない)変数を探しに行く
// クロージャ関数を実現するために必要な処理.
// 見つかった場合は該当する Compiler 構造体の locals の idx を, そうでなければ -1 を返す.
// 返り値は OP_{GET,SET}_UPVALUE 命令のオペランドとなる.
static int resolveUpvalue(Compiler *compiler, Token *name) {
  if (compiler->enclosing == NULL) return -1; // 再起の終了条件 == トップレベルに到達した

  // 関数を囲んでいる(一つ外側のCompiler構造体に)変数が locals に登録されているか調べる
  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true; // クロージャにキャプチャされたフラグをON.
    // 外側を囲んでいる関数なら isLocal=true で upvalue として登録 = 一つ外側にある変数である
    return addUpvalue(compiler, (uint8_t) local, true);
  }

  // 変数が見つかるまでさらに外側の関数のlocalsを再帰して検索していく.
  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    // resolveUpvalue の返り値で発見されたアドレス(idx)は間接的な参照値の扱いである.
    // よって isLocal=false.
    // see: https://www.craftinginterpreters.com/closures.html#flattening-upvalues
    // see: https://www.craftinginterpreters.com/image/closures/linked-upvalues.png
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

// markInitialized は現在解析中の宣言された変数を初期化済みとしてマークする
static void markInitialized() {
  // コンパイラがトップレベルを解析中なら, それはグローバル変数なので何もしない
  // lox ではグローバル変数はlate binding なので実行時に初期化済みであれば問題ない仕様のため.
  if (current->scopeDepth == 0)
    return;

  // 変数が定義されたスコープ(その変数を所有するスコープ)の深さを記録する.
  // この depth の値が -1 なら初期化済みというマークとなる.
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

// 関数の引数リストを解析する.
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
  // infixルールなので, この関数が呼ばれた時点で左辺値はすでにコンパイル済みの状態.
  // 実行時にはその結果がすでにスタックの銭湯にあるので, 偽とき右辺値を評価する必要はない.
  int endJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP); // 左辺値はもう不要なのでPOPする.
  parsePrecedence(PREC_AND);

  patchJump(endJump); // jump先アドレスをpatchする
  // 右辺値の値は and 式の結果となるので POP しないことに注意.
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

// 関数呼び出し`()`はinfix演算子.
static void call(bool canAssign) {
  uint8_t argCount = argumentList(); // 引数を解析
  emitBytes(OP_CALL, argCount); // 関数呼び出し命令をemit
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
  // 左辺値が真なら, 残りの評価は不要
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP); // 真の場合は終わりまでJUMPする

  patchJump(elseJump);
  emitByte(OP_POP); // 左辺値の値は不要なのでPOPする

  parsePrecedence(PREC_OR); // 後続の右辺値を評価する
  patchJump(endJump); // 終了JUMP先のアドレスをpatchする
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
  if (arg != -1) {
    // locals配列のidxが -1 ではないならローカル変数
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    // クロージャ変数である可能性を考え, 外部ブロックの変数を探してキャプチャしにいく
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    // -1 はローカル変数には登録されていない名前である = グローバル変数である
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
    [TOKEN_LEFT_PAREN]    = {grouping, call, PREC_CALL}, // パーサーが式に続いて`(`を発見するとそれは関数CALL演算子である.
    [TOKEN_RIGHT_PAREN]   = {NULL, NULL, PREC_NONE}, // )
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

// function は関数をコンパイルする.
static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope(); // [no-end-scope]

  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  if (!check(TOKEN_RIGHT_PAREN)) { // ) がなければ, 関数には引数がある.
    do {
      current->function->arity++; // 引数の数をインクリメント.
      if (current->function->arity > 255) {
        errorAtCurrent("Can't have more than 255 parameters.");
      }
      uint8_t constant = parseVariable("Expect parameter name.");
      defineVariable(constant);
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");

  // 関数本体の処理をコンパイル
  block();

  ObjFunction *function = endCompiler();

  // クロージャ関数シンボルを定数Chunkに登録してemit.
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));
  // 可変長のupValueオペランド(2*n Bytes)が続く
  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0); // isLocal かどうか
    emitByte(compiler.upvalues[i].index);           // upValue の idx 値
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

// funDeclaration は関数宣言を解析する.
static void funDeclaration() {
  // loxでは関数はファーストクラスの値なので, 関数宣言は変数を作成しその中に定義を格納するという処理になる.
  // トップレベルならグローバル変数, それ以外ならローカル変数に関数を格納する.
  uint8_t global = parseVariable("Expect function name.");
  markInitialized(); // 関数定義内で自分自身を参照(再起関数の定義)ができるように, 解析前に初期化済みフラグを付与しておく
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
  // for(`初期値`; ...) の箇所のコンパイル
  if (match(TOKEN_SEMICOLON)) {
    // 初期化子なし
  } else if (match(TOKEN_VAR)) {
    // var i = 0; とかする場合
    varDeclaration();
  } else {
    // 既存変数を用いた式など
    expressionStatement();
  }

  int loopStart = currentChunk()->count;
  int exitJump = -1;
  // for(; `条件式`; ...) のコンパイル
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    // Jump out of the loop if the condition is false.
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // 条件式の結果は不要なのでPOPする
  }

  // for(;;`更新式`) のコンパイル
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

  statement(); // for の中身のコンパイル
  emitLoop(loopStart);

  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }

  endScope();
}

// ifStatement は if文をコンパイルする
static void ifStatement() {
  // if `(条件式)` をコンパイルする
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition."); // [paren]

  // 条件が偽の場合はif文の終了アドレスまでジャンプする
  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP); // 条件式の結果はもういらないのでPOPする(真のとき)
  statement(); // then節

  // else節を実行してはいけないので else の後にJUMPする
  int elseJump = emitJump(OP_JUMP);

  patchJump(thenJump); // 仮置きしたオペランドを更新する
  emitByte(OP_POP); // 条件式の結果はもういらないのでPOPする(偽のとき)

  // else節があるなら再び文の解析に入る -> else | if (...) という流れで else if が実現できる.
  if (match(TOKEN_ELSE)) statement();
  // else を実行しなかった場合の飛び先はこちら
  patchJump(elseJump);
}

static void printStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  emitByte(OP_PRINT);
}

// returnStatement は return文を解析する.
static void returnStatement() {
  if (current->type == TYPE_SCRIPT) {
    error("Can't return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON)) {
    // return; の場合は暗黙的returnと同じ処理.
    emitReturn();
  } else {
    if (current->type == TYPE_INITIALIZER) {
      error("Can't return a value from an initializer.");
    }

    // 返り値のがある場合はそれをPUSHしたあとに0P_RETURN.
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
    emitByte(OP_RETURN);
  }
}

static void whileStatement() {
  int loopStart = currentChunk()->count; // 開始アドレスを取得しておく
  // while `(条件式)` のコンパイル
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  // 条件式が偽なら終わりまでJUMP
  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP); // 条件式の評価結果は不要なのでPOP(真の場合)
  statement();
  emitLoop(loopStart); // 開始アドレスまで戻る

  patchJump(exitJump);
  emitByte(OP_POP); // 条件式の評価結果は不要なのでPOP(偽の場合)
}

static void synchronize() {
  // 文の解析終わりにエラー抑制フラグをOFFにする
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
    // 関数宣言
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

  // エラーがなければコンパイルした関数オブジェクトの参照を返す.
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
