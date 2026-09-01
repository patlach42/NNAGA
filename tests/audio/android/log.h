#ifndef NNAGA_TEST_ANDROID_LOG_H
#define NNAGA_TEST_ANDROID_LOG_H

#define ANDROID_LOG_ERROR 6

inline int __android_log_print(int, const char*, const char*, ...) { return 0; }

#endif
