#include <stdlib.h>

#include "compiler.h"
#include "memory.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>
#include "debug.h"
#endif

#define GC_HEAP_GROW_FACTOR 2

// reallocate は clox 全体で使用されるメモリ確保ユーティリティ関数.
// この関数を呼び出すときGCが起動することがある.
// - newSize = 0 のときメモリを解放する.
void *reallocate(void *pointer, size_t oldSize, size_t newSize) {
  // 確保されたヒープメモリのサイズを更新する
  vm.bytesAllocated += newSize - oldSize;
  // GC call-collect
  if (newSize > oldSize) {
#ifdef DEBUG_SRESS_GC  // GCのデバッグ時に有効化する
    collectGarbage();
#endif
    // しきい値を超えたらGCを起動する
    if (vm.bytesAllocated > vm.nextGC) {
      collectGarbage();
    }
  }

  if (newSize == 0) {
    free(pointer);
    return NULL;
  }

  void *result = realloc(pointer, newSize);
  if (result == NULL) exit(1); // out of memory

  return result;
}

void markObject(Obj *object) {
  if (object == NULL) return;
  // すでに追跡された痕跡がある(灰色 or 黒色)なら何もしない
  if (object->isMarked) return;

#ifdef DEBUG_LOG_GC
  printf("%p mark ", (void*)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif

  // GCに追跡された痕跡をマークする
  // 注意:このフラグは黒色に塗りつぶすことを意味しない
  object->isMarked = true;

  if (vm.grayCapacity < vm.grayCount + 1) {
    vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
    // 灰色オブジェクトを管理するStackの realloc はOS固有のものを直接使う.
    // これはGCのためのスタック伸長時にGCが起動してしまうような状態を避けるため.
    vm.grayStack = (Obj **) realloc(vm.grayStack, sizeof(Obj *) * vm.grayCapacity);

    if (vm.grayStack == NULL) exit(1);
  }

  // このStackに追加されることが、灰色にマークされることである
  // isGray のようなフラグはないことに注意
  vm.grayStack[vm.grayCount++] = object;
}

// markValue は値が到達可能かチェックする
void markValue(Value value) {
  // Obj 以外(数値, bool, nil)は内部的に即値でありヒープに確保されないので IS_OBJ で判別する.
  if (IS_OBJ(value)) markObject(AS_OBJ(value));
}

static void markArray(ValueArray *array) {
  for (int i = 0; i < array->count; i++) {
    markValue(array->values[i]);
  }
}

// grayStack から取り出された灰色オブジェクトを黒く塗っていく(GC処理)
static void blackenObject(Obj *object) {
#ifdef DEBUG_LOG_GC
  printf("%p blacken ", (void*)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif
  // オブジェクトの種類ごとにmarkしていく
  switch (object->type) {
    case OBJ_BOUND_METHOD: {
      ObjBoundMethod *bound = (ObjBoundMethod *) object;
      markValue(bound->receiver);
      markObject((Obj *) bound->method);
      break;
    }
    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *) object;
      markObject((Obj *) klass->name);
      markTable(&klass->methods);
      break;
    }
    case OBJ_CLOSURE: {
      ObjClosure *closure = (ObjClosure *) object;
      markObject((Obj *) closure->function); // 参照していく関数をmark
      // キャプチャしている upvalues を mark していく
      for (int i = 0; i < closure->upvalueCount; i++) {
        markObject((Obj *) closure->upvalues[i]);
      }
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction *function = (ObjFunction *) object;
      markObject((Obj *) function->name);
      markArray(&function->chunk.constants);
      break;
    }
    case OBJ_INSTANCE: {
      ObjInstance *instance = (ObjInstance *) object;
      markObject((Obj *) instance->klass);
      markTable(&instance->fields);
      break;
    }
    case OBJ_UPVALUE:
      markValue(((ObjUpvalue *) object)->closed);
      break;
    case OBJ_NATIVE:
    case OBJ_STRING:
      break;
  }
}

static void freeObject(Obj *object) {
#ifdef DEBUG_LOG_GC
  printf("%p free type %d\n", (void*)object, object->type);
#endif

  switch (object->type) {
    case OBJ_BOUND_METHOD:
      FREE(ObjBoundMethod, object);
      break;
    case OBJ_CLASS: {
      ObjClass *klass = (ObjClass *) object;
      freeTable(&klass->methods);
      FREE(ObjClass, object);
      break;
    } // [braces]
    case OBJ_CLOSURE: {
      // クロージャ関数はラップしている関数までは解放しないことに注意.
      ObjClosure *closure = (ObjClosure *) object;
      FREE_ARRAY(ObjUpvalue*, closure->upvalues,
                 closure->upvalueCount);
      FREE(ObjClosure, object);
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction *function = (ObjFunction *) object;
      freeChunk(&function->chunk);
      FREE(ObjFunction, object);
      break;
    }
    case OBJ_INSTANCE: {
      ObjInstance *instance = (ObjInstance *) object;
      freeTable(&instance->fields);
      FREE(ObjInstance, object);
      break;
    }
    case OBJ_NATIVE:
      FREE(ObjNative, object);
      break;
    case OBJ_STRING: {
      ObjString *string = (ObjString *) object;
      FREE_ARRAY(char, string->chars, string->length + 1);
      FREE(ObjString, object);
      break;
    }
    case OBJ_UPVALUE:
      // 複数のクロージャ関数から参照されている可能性もあるため, キャプチャした変数は開放しない.
      FREE(ObjUpvalue, object);
      break;
  }
}

static void markRoots() {
  for (Value *slot = vm.stack; slot < vm.stackTop; slot++) {
    markValue(*slot);
  }

  for (int i = 0; i < vm.frameCount; i++) {
    markObject((Obj *) vm.frames[i].closure);
  }

  // クロージャにキャプチャされたupvaluesもマークする
  for (ObjUpvalue *upvalue = vm.openUpvalues;
       upvalue != NULL;
       upvalue = upvalue->next) {
    markObject((Obj *) upvalue);
  }

  markTable(&vm.globals);  // グローバル変数
  markCompilerRoots();  // compilerチェーンを辿って関数オブジェクトをmarkしていく
  markObject((Obj *) vm.initString);
}

// grayStack(灰色オブジェクトの集合)が空になるまでオブジェクトを取り出し, 参照を走査し黒く塗っていく.
// 白色オブジェクトが発見されたら灰色に塗る.
static void traceReferences() {
  while (vm.grayCount > 0) {
    Obj *object = vm.grayStack[--vm.grayCount];
    blackenObject(object);
  }
}

// リンクリストを走査していき `isMarked = false` なオブジェクトをリストから外して解放する
static void sweep() {
  Obj *previous = NULL;
  Obj *object = vm.objects;
  while (object != NULL) {
    if (object->isMarked) {
      // 黒色オブジェクトである
      object->isMarked = false; // フラグをもとに戻す
      previous = object;
      object = object->next;
    } else {
      // 白色オブジェクトである
      Obj *unreached = object;
      // unreached を連結リストから外す
      object = object->next;
      if (previous != NULL) {
        previous->next = object;
      } else {
        vm.objects = object;
      }

      // 解放する
      freeObject(unreached);
    }
  }
}

// collectGarbage は mark-sweep GC を開始する起点.
void collectGarbage() {
#ifdef DEBUG_LOG_GC
  printf("-- gc begin\n");
  size_t before = vm.bytesAllocated;
#endif

  markRoots();
  traceReferences();
  // 文字列テーブルはmark終了とsweepが実施される間にチェックする
  tableRemoveWhite(&vm.strings);
  sweep();

  vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;

#ifdef DEBUG_LOG_GC
  printf("-- gc end\n");
  printf("   collected %zu bytes (from %zu to %zu) next at %zu\n",
         before - vm.bytesAllocated, before, vm.bytesAllocated,
         vm.nextGC);
#endif
}

void freeObjects() {
  Obj *object = vm.objects;
  while (object != NULL) {
    Obj *next = object->next;
    freeObject(object);
    object = next;
  }

  free(vm.grayStack);
}
