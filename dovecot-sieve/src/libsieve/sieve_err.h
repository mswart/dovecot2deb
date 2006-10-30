/*
 * sieve_err.h:
 * This file used to be automatically generated from sieve_er.et.
 */
struct et_list;

#define SIEVE_FAIL                               (-1237848064L)
#define SIEVE_NOT_FINALIZED                      (-1237848063L)
#define SIEVE_PARSE_ERROR                        (-1237848062L)
#define SIEVE_RUN_ERROR                          (-1237848061L)
#define SIEVE_INTERNAL_ERROR                     (-1237848060L)
#define SIEVE_NOMEM                              (-1237848059L)
#define SIEVE_DONE                               (-1237848058L)
extern const struct error_table et_siev_error_table;
extern void initialize_siev_error_table(void);

/* For compatibility with Heimdal */
extern void initialize_siev_error_table_r(struct et_list **list);

#define ERROR_TABLE_BASE_siev (-1237848064L)

/* for compatibility with older versions... */
#define init_siev_err_tbl initialize_siev_error_table
#define siev_err_base ERROR_TABLE_BASE_siev
