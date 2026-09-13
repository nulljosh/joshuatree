#ifndef JSON_H
#define JSON_H
/* Not a JSON parser, a string-value grabber: finds "key":"...", copies the
   value out with \" \\ \n \t \r unescaped, stops at the closing quote.
   Enough to read one field out of an LLM API's JSON response, nothing
   more, no numbers/arrays/nesting awareness. */
unsigned int json_extract_string(const char *json, const char *key, char *out, unsigned int maxlen);

/* Writes s into out with " \ and control characters escaped, for building
   a JSON string value to send. Returns the number of bytes written. */
unsigned int json_escape(const char *s, char *out, unsigned int maxlen);
#endif
