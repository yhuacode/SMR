/*
 * kvstore_htable.c
 *
 *  Created on: Mar 23, 2014
 *      Author: fumin
 */

#include "destor.h"
#include "index.h"

typedef unsigned char* kvpair;

#define get_key(kv) (kv)
#define get_value(kv) ((int64_t*)(kv+destor.index_key_size))
extern struct structdestor destor;
static GHashTable *htable;

static int32_t kvpair_size;

/*
 * Create a new kv pair.
 */
static kvpair new_kvpair_full(unsigned char* key){
    kvpair kvp = (kvpair)malloc(kvpair_size);
    memcpy(get_key(kvp), key, destor.index_key_size);
    int64_t* values = get_value(kvp);
    int i;
    for(i = 0; i<destor.index_value_length; i++){
    	values[i] = TEMPORARY_ID;
    }
    return kvp;
}

static kvpair new_kvpair(){
	 kvpair kvp = (kvpair)malloc(kvpair_size);
	 int64_t* values = get_value(kvp);
	 int i;
	 for(i = 0; i<destor.index_value_length; i++){
		 values[i] = TEMPORARY_ID;
	 }
	 return kvp;
}

/*
 * IDs in value are in FIFO order.
 * value[0] keeps the latest ID.
 */
static void kv_update(kvpair kv, int64_t id){
    int64_t* value = get_value(kv);
	memmove(&value[1], value,
			(destor.index_value_length - 1) * sizeof(int64_t));
	value[0] = id;
}

static inline void free_kvpair(kvpair kvp){
	free(kvp);
}

void init_kvstore_htable(){
    kvpair_size = destor.index_key_size + destor.index_value_length * 8;

    if(destor.index_key_size >=4)
    	htable = g_hash_table_new_full(g_int_hash, (GEqualFunc)g_feature_equal,
			(GDestroyNotify)free_kvpair, NULL);
    else
    	htable = g_hash_table_new_full((GHashFunc)g_feature_hash, (GEqualFunc)g_feature_equal,
			(GDestroyNotify)free_kvpair, NULL);

	sds indexpath = sdsdup(destor.working_directory);
	indexpath = sdscat(indexpath, "index/htable");

	/* Initialize the feature index from the dump file. */
	FILE *fp;
	if ((fp = fopen(indexpath, "r"))) {
		/* The number of features */
		int key_num;
		fread(&key_num, sizeof(int), 1, fp);
		for (; key_num > 0; key_num--) {
			/* Read a feature */
			kvpair kv = new_kvpair();
			fread(get_key(kv), destor.index_key_size, 1, fp);

			/* The number of segments/containers the feature refers to. */
			int id_num, i;
			fread(&id_num, sizeof(int), 1, fp);
			assert(id_num <= destor.index_value_length);

			for (i = 0; i < id_num; i++)
				/* Read an ID */
				fread(&get_value(kv)[i], sizeof(int64_t), 1, fp);

			g_hash_table_insert(htable, get_key(kv), kv);
		}
		fclose(fp);
	}

	sdsfree(indexpath);
}

void close_kvstore_htable() {
	sds indexpath = sdsdup(destor.working_directory);
	indexpath = sdscat(indexpath, "index/htable");

	FILE *fp;
	if ((fp = fopen(indexpath, "w")) == NULL) {
		perror("Can not open index/htable for write because:");
		exit(1);
	}

	NOTICE("flushing hash table!");
	int key_num = g_hash_table_size(htable);
	fwrite(&key_num, sizeof(int), 1, fp);

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, htable);
	while (g_hash_table_iter_next(&iter, &key, &value)) {

		/* Write a feature. */
		kvpair kv = (kvpair)value;
		if(fwrite(get_key(kv), destor.index_key_size, 1, fp) != 1){
			perror("Fail to write a key!");
			exit(1);
		}

		/* Write the number of segments/containers */
		if(fwrite(&destor.index_value_length, sizeof(int), 1, fp) != 1){
			perror("Fail to write a length!");
			exit(1);
		}
		int i;
		for (i = 0; i < destor.index_value_length; i++)
			if(fwrite(&get_value(kv)[i], sizeof(int64_t), 1, fp) != 1){
				perror("Fail to write a value!");
				exit(1);
			}

	}

	/* It is a rough estimation */
	destor.index_memory_footprint = g_hash_table_size(htable)
			* (destor.index_key_size + sizeof(int64_t) * destor.index_value_length + 4);

	fclose(fp);

	NOTICE("flushing hash table successfully!");

	sdsfree(indexpath);

	g_hash_table_destroy(htable);
}

/*
 * For top-k selection method.
 */
int64_t* kvstore_htable_lookup(unsigned char* key) {
	kvpair kv = (kvpair)g_hash_table_lookup(htable, key);
	return kv ? get_value(kv) : NULL;
}

/* courageJ
 */
void kvstore_htable_update(unsigned char* key, int64_t id) {
	//origin chunk
	kvpair kv = (kvpair)g_hash_table_lookup(htable, key);
	if (!kv) {
		kv = new_kvpair_full(key);
		g_hash_table_replace(htable, get_key(kv), kv);
	}
	kv_update(kv, id);

	//Firstly, find chunk count
	fingerprint chunk_count_fp;
	fingerprint new_fp;
	fingerprint exist_fp;
	for (int i = 0; i < 20; i++) {
		chunk_count_fp[i] = key[i];
		new_fp[i] = key[i];
		exist_fp[i] = key[i];
	}
	chunk_count_fp[0] = (unsigned char)(254);
/*
                char code[41];
                hash2code((unsigned char*)exist_fp, code);
                code[40] = 0;
                //printf ("182L: code = %s\n",  code);
*/

	int64_t chunk_count = 0;
	int64_t *chunk_count_pt = kvstore_htable_lookup(chunk_count_fp);
	if (!chunk_count_pt) {
		chunk_count = 0;
	/*
	char code[41];
		hash2code((unsigned char*)chunk_count_fp, code);
		code[40] = 0;
		//printf ("190L:  code = %s\n",  code);*/
        }
	else {
		chunk_count = *chunk_count_pt;
		/*
		char code[41];
		hash2code((unsigned char*)chunk_count_fp, code);
		code[40] = 0;
		//printf ("194L: code = %s, chunk_count = %d\n",  code, chunk_count);*/
		
	}
	//printf ("WJ: chunk_count = %d\n", chunk_count);
	//Secondly, find every count, and determine whether the same id is added.
	for (int i = 1; i <= chunk_count; i++) {
		exist_fp[0] = (char)i;
		int64_t *exist_id_pt = kvstore_htable_lookup(exist_fp);
		if (!exist_id_pt) {/*
			char code[41];
			hash2code((unsigned char*)exist_fp, code);
			code[40] = 0;*/
			//printf ("200L: bug in i = %d, code = %s\n", i, code);
			//printf ("WJ: c->flag = %d\n", c->flag);
			chunk_count = i - 1;
			break;
			exit(-1);
		}
		int64_t exist_id = *exist_id_pt;
		if (exist_id == id) {
			return;
		}
	}
	//Thirdly, add new chunk with[0] = count, into hash table
	// count starts from 1
	chunk_count ++;
	//printf ("WJ2: chunk_count = %d\n", chunk_count);
	new_fp[0] = (unsigned char)chunk_count;
	if (chunk_count > 254) {
		char code[41];
		hash2code((unsigned char*)exist_fp, code);
		code[40] = 0;
		printf ("218L: bug:fp_count overflow: chunk_count = %lld, code = %s\n", chunk_count, code);
		return;
//		exit(-1);
	}
	fingerprint *new_fp_pt = &new_fp;
	kv = (kvpair)g_hash_table_lookup(htable, new_fp_pt);
	if (!kv) {
		kv = new_kvpair_full((unsigned char*)new_fp);
		g_hash_table_replace(htable, get_key(kv), kv);
	}
	kv_update(kv, id);
	//Fourthly, update chunk count
	fingerprint *chunk_count_fp_pt = &chunk_count_fp;
	kv = (kvpair)g_hash_table_lookup(htable, chunk_count_fp_pt);
	if (!kv) {
		kv = new_kvpair_full((unsigned char *)chunk_count_fp);
		g_hash_table_replace(htable, get_key(kv), kv);
	}
	kv_update(kv, chunk_count);
/*	
	//char code[41];
	hash2code((unsigned char*)new_fp, code);
	code[40] = 0;
	printf ("237L: new_fp = %s, id = %lld\n", code, id);

	hash2code((unsigned char*)chunk_count_fp, code);
	printf ("240L: chunk_count_fp = %s, chunk_count = %lld\n", code, chunk_count);
*/	
}

/* Remove the 'id' from the kvpair identified by 'key' */
void kvstore_htable_delete(unsigned char* key, int64_t id){
	kvpair kv = (kvpair)g_hash_table_lookup(htable, key);
	if(!kv)
		return;

	int64_t *value = get_value(kv);
	int i;
	for(i=0; i<destor.index_value_length; i++){
		if(value[i] == id){
			value[i] = TEMPORARY_ID;
			/*
			 * If index exploits physical locality,
			 * the value length is 1. (correct)
			 * If index exploits logical locality,
			 * the deleted one should be in the end. (correct)
			 */
			/* NOTICE: If the backups are not deleted in FIFO order, this assert should be commented */
			assert((i == destor.index_value_length - 1)
					|| value[i+1] == TEMPORARY_ID);
			if(i < destor.index_value_length - 1 && value[i+1] != TEMPORARY_ID){
				/* If the next ID is not TEMPORARY_ID */
				memmove(&value[i], &value[i+1], (destor.index_value_length - i - 1) * sizeof(int64_t));
			}
			break;
		}
	}

	/*
	 * If all IDs are deleted, the kvpair is removed.
	 */
	if(value[0] == TEMPORARY_ID){
		/* This kvpair can be removed. */
		g_hash_table_remove(htable, key);
	}
}