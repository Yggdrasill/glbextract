#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <error.h>
#include <search.h>
#include "glbextract.h"

int strcompar(const void *s1, const void *s2)
{
  return strcmp(*(char **)s1, *(char **)s2);
}

int calculate_key_pos(size_t len)
{
  return 25 % len;
}

void reset_state(struct State *state)
{
  state->key_pos = calculate_key_pos(strlen(DEFAULT_KEY) );
  state->prev_byte = DEFAULT_KEY[state->key_pos];

  return;
}

void die(const char *str)
{
  error(errno ? errno : EXIT_FAILURE, errno, "%s", str ? str : "");

  return;
}

void warn(const char *str, const char *filename)
{
  fprintf(stderr, "warn: %s %s\n", str, filename);

  return;
}

void print_usage(char *name)
{
  printf("%s %s\n", name, HELP_TEXT);

  return;
}

void args_tokenize(char *arg, struct Tokens *tokens)
{
  char *last;
  int i;

  i = 0;
  while(i < MAX_FILES && (tokens->table[i] = strtok_r(arg, ",", &last) ) ) {
    tokens->ntokens++;
    arg = NULL;
    i++;
  }

  return;
}

int args_parse(int argc, char **argv, struct Tokens *tokens)
{
  int arg;
  int mask;

  mask = 0;

  while( (arg = getopt(argc, argv, "hlxe:") ) != -1) {
    switch(arg) {
      case 'l':
        mask |= ARGS_LIST;
        break;
      case 'x':
        mask |= ARGS_EXTA;
        break;
      case 'e':
        mask |= ARGS_EXTS;
        args_tokenize(optarg, tokens);
        break;
      case 'h':
      default:
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
  }

  if(arg == -1 && argc == 2) mask = ARGS_LIST;

  return mask;
}

char *buffer_copy_fat(struct FATable *fat, char *buffer)
{
  memcpy(&fat->flags, buffer, READ8_MAX);
  buffer += READ8_MAX;
  memcpy(&fat->offset, buffer, READ8_MAX);
  buffer += READ8_MAX;
  memcpy(&fat->length, buffer, READ8_MAX);
  buffer += READ8_MAX;
  memcpy(fat->filename, buffer, MAX_FILENAME_LEN);
  buffer += MAX_FILENAME_LEN;

  return buffer;
}

static int decrypt_varlen(struct State *state, void *data, size_t size)
{
  char *current_byte;
  char *prev_byte;
  char *byte_data;
  uint8_t *key_pos;

  current_byte = &state->current_byte;
  prev_byte = &state->prev_byte;
  key_pos = &state->key_pos;
  byte_data = (char *)data;

  size_t i;
  for(i = 0; i < size; i++) {
    *current_byte = byte_data[i];
    byte_data[i] = *current_byte - DEFAULT_KEY[*key_pos] - *prev_byte;
    byte_data[i] &= 0xFF;
    (*key_pos)++;
    *key_pos %= strlen(DEFAULT_KEY);
    *prev_byte = *current_byte;
  }

  return i;
}

int decrypt_uint32(struct State *state, uint32_t *data32)
{
  return decrypt_varlen(state, data32, READ8_MAX);
}

int decrypt_filename(struct State *state, char *str)
{
  *(str + MAX_FILENAME_LEN - 1) = '\0';
  return decrypt_varlen(state, str, MAX_FILENAME_LEN - 1);
}

int decrypt_fat_single(struct State *state, struct FATable *fat)
{
  int retval;

  reset_state(state);

  retval = decrypt_uint32(state, &fat->flags);
  retval += decrypt_uint32(state, &fat->offset);
  retval += decrypt_uint32(state, &fat->length);
  retval += decrypt_filename(state, fat->filename);

  return retval;
}

int decrypt_fat_many(struct State *state,
                    struct FATable *ffat,
                    char *buffer,
                    uint32_t nfiles)
{
  struct FATable *end;

  char *newptr;

  int retval;

  end = ffat + nfiles;
  retval = 0;

  for( ; ffat < end; ffat++) {
    newptr = buffer_copy_fat(ffat, buffer);
    retval += decrypt_fat_single(state, ffat);
    buffer = newptr;
  }

  return retval;
}

int decrypt_file(struct State *state, char *str, size_t length)
{
  reset_state(state);
  *(str + length) = '\0';
  return decrypt_varlen(state, str, length - 1);
}

struct FATable *fat_array_init(uint32_t nfiles)
{
  struct FATable *ffat;

  ffat = malloc(sizeof(*ffat) * nfiles);

  return ffat;
}

void fat_array_free(struct FATable **ffat)
{
  if(ffat) {
    free(*ffat);
    *ffat = NULL;
  }

  return;
}

void fat_names_fix(struct FATable *ffat, uint32_t nfiles)
{
  struct FATable *end;

  int i;

  end = ffat + nfiles;

  for(i = 0; ffat < end; ffat++, i++) {
    if(!ffat->filename[0]) {
      snprintf(ffat->filename, MAX_FILENAME_LEN, "%d", i);
    }
  }

  return;
}

void fat_flag_extraction(struct FATable *ffat,
                        struct Tokens *tokens,
                        uint32_t nfiles,
                        int arg_mask)
{
  struct FATable *end;

  char **result;
  char *str;

  end = ffat + nfiles;

  for( ; ffat < end; ffat++) {
    str = ffat->filename;
    result = lfind(&str, tokens->table, &tokens->ntokens,
                  sizeof(*tokens->table), strcompar);

    if(result || (arg_mask & ARGS_EXTA) ) {
      ffat->extract = 1;
    } else {
      ffat->extract = 0;
    }
  }

  return;
}

void fat_array_print(struct FATable *ffat, uint32_t nfiles)
{
  struct FATable *end;

  end = ffat + nfiles;

  for( ; ffat < end; ffat++) {
    printf("%*s %c %u bytes\n", -MAX_FILENAME_LEN, ffat->filename,
          ffat->flags ? 'E' : 'N', ffat->length);
  }

  return;
}

struct FATable *fat_find_largest(struct FATable *ffat, uint32_t nfiles)
{
  struct FATable *end;
  struct FATable *largest;

  end = ffat + nfiles;
  largest = ffat;

  for( ; ffat < end; ffat++) {
    if(ffat->length > largest->length) largest = ffat;
  }

  return largest;
}

int main(int argc, char **argv)
{
  FILE *glb;

  struct FATable hfat = {0};
  struct FATable *ffat = {0};
  struct FATable *largest = {0};
  struct State state = {0};
  struct Tokens tokens = {0};

  ssize_t bytes;

  int arg_mask;
  int fd;

  char *buffer;
  buffer = NULL;

  if(argc < 2) {
    print_usage(argv[0]);
    die("Too few arguments");
  }

  arg_mask = args_parse(argc, argv, &tokens);

  if(!argv[optind]) {
    die("No input file");
  }

  glb = fopen(argv[optind], "rb");

  if(!glb) {
    die(argv[optind]);
  }

  fd = fileno(glb);
  buffer = malloc(FAT_SIZE);

  if(!buffer) {
    die(argv[0]);
  }

  bytes = pread(fd, buffer, FAT_SIZE, 0);

  if(bytes == -1) {
    die(argv[optind]);
  } else if(bytes != FAT_SIZE) {
    die("Couldn't read header FAT. Giving up.");
  } else if(strncmp(RAW_HEADER, buffer, READ8_MAX) ) {
    die("Not a GLB file!");
  }

  reset_state(&state);
  buffer_copy_fat(&hfat, buffer);
  decrypt_fat_single(&state, &hfat);

  ffat = fat_array_init(hfat.offset);

  if(!ffat) {
    die(argv[0]);
  }

  buffer = realloc(buffer, hfat.offset * FAT_SIZE);

  if(!buffer) {
    die(argv[0]);
  }

  bytes = pread(fd, buffer, hfat.offset * FAT_SIZE, FAT_SIZE);

  if(bytes == -1) {
    die(argv[optind]);
  } else if(bytes != hfat.offset * FAT_SIZE) {
    die("Couldn't read file allocation tables. Giving up.");
  }

  decrypt_fat_many(&state, ffat, buffer, hfat.offset);
  fat_names_fix(ffat, hfat.offset);

  if(arg_mask & ARGS_LIST) fat_array_print(ffat, hfat.offset);


  free(buffer);
  fclose(glb);
  fat_array_free(&ffat);

  return 0;
}
