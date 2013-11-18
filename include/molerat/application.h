#ifndef MOLERAT_APPLICATION_H
#define MOLERAT_APPLICATION_H

#define PRIVATE_SIGNAL SIGUSR1

void application_prepare(void);
void application_assert_prepared(void);

bool_t application_run(void);
void application_stop(void);

#endif

