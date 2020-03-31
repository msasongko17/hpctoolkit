typedef struct reuseBBEntry{
  uint64_t time __attribute__((aligned(CACHE_LINE_SZ)));
  int tid;
  int core_id;
  int active_flag;
} ReuseBBEntry_t;

typedef struct reuseHashTableStruct{
  volatile uint64_t counter __attribute__((aligned(64)));
  struct reuseBBEntry hashTable[503];
  //struct SharedData * hashTable;
} ReuseHashTable_t;

extern ReuseHashTable_t reuseBulletinBoard;
