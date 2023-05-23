#include <stdlib.h>

#include "chunk.h"
#include "memory.h"
#include "vm.h"

void initChunk(Chunk* chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  initValueArray(&chunk->constants);
}

void freeChunk(Chunk* chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity); // reallocate(chunk->code, sizeof(uint8_t) * (chunk->capacity), 0)
    FREE_ARRAY(int, chunk->lines, chunk->capacity); // reallocate(chunk->lines, sizeof(int) * (chunk->capacity), 0)
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}


/* chunks of bytecode write-chunk < chunks of bytecode write-chunk-with-line
void writechunk(chunk* chunk, uint8_t byte) {
*/
void writeChunk(Chunk* chunk, uint8_t byte, int line) {
  // メモリ容量が足りないなら新しく確保する
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    chunk->capacity = GROW_CAPACITY(oldCapacity);
    chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);
    chunk->lines = GROW_ARRAY(int, chunk->lines, oldCapacity, chunk->capacity);
  }

  chunk->code[chunk->count] = byte;
  chunk->lines[chunk->count] = line;
  chunk->count++;
}

int addConstant(Chunk* chunk, Value value) {
  push(value);
  writeValueArray(&chunk->constants, value);
  pop();
  return chunk->constants.count - 1;
}
