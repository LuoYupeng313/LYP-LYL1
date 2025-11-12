/**
 * @file servo.c
 * @note Copyright (C) 2011 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "linreg.h"
#include "ntpshm.h"
#include "nullf.h"
#include "pi.h"
#include "refclock_sock.h"
#include "servo_private.h"
#include "util.h"

#include "print.h"
#include "servo.h"

struct servo *servo_create(struct config *cfg, enum servo_type type,
			   double fadj, int max_ppb, int sw_ts)
{
	double servo_first_step_threshold;
	double servo_step_threshold;
	int servo_max_frequency;
	struct servo *servo;

	switch (type) {
	case CLOCK_SERVO_PI:
		servo = pi_servo_create(cfg, fadj, sw_ts);
		break;
	case CLOCK_SERVO_LINREG:
		servo = linreg_servo_create(fadj);
		break;
	case CLOCK_SERVO_NTPSHM:
		servo = ntpshm_servo_create(cfg);
		break;
	case CLOCK_SERVO_NULLF:
		servo = nullf_servo_create();
		break;
	case CLOCK_SERVO_REFCLOCK_SOCK:
		servo = refclock_sock_servo_create(cfg);
		break;
	default:
		return NULL;
	}

	if (!servo)
		return NULL;

	servo_step_threshold = config_get_double(cfg, NULL, "step_threshold");
	if (servo_step_threshold > 0.0) {
		servo->step_threshold = servo_step_threshold * NSEC_PER_SEC;
	} else {
		servo->step_threshold = 0.0;
	}

	servo_first_step_threshold =
		config_get_double(cfg, NULL, "first_step_threshold");

	if (servo_first_step_threshold > 0.0) {
		servo->first_step_threshold =
			servo_first_step_threshold * NSEC_PER_SEC;
	} else {
		servo->first_step_threshold = 0.0;
	}

	servo_max_frequency = config_get_int(cfg, NULL, "max_frequency");
	servo->max_frequency = max_ppb;
	if (servo_max_frequency && servo->max_frequency > servo_max_frequency) {
		servo->max_frequency = servo_max_frequency;
	}

	servo->first_update = 1;
	servo->offset_threshold = config_get_int(cfg, NULL, "servo_offset_threshold");
	servo->num_offset_values = config_get_int(cfg, NULL, "servo_num_offset_values");
	servo->curr_offset_values = servo->num_offset_values;

	return servo;
}

void servo_destroy(struct servo *servo)
{
	servo->destroy(servo);
}

static int check_offset_threshold(struct servo *s, int64_t offset)
{
	long long int abs_offset = llabs(offset);

	if (s->offset_threshold) {
		if (abs_offset < s->offset_threshold) {
			if (s->curr_offset_values)
				s->curr_offset_values--;
		} else {
			s->curr_offset_values = s->num_offset_values;
		}
		return s->curr_offset_values ? 0 : 1;
	}
	return 0;
}

double servo_sample(struct servo *servo,
		    int64_t offset,
		    uint64_t local_ts,
		    double weight,
		    enum servo_state *state)
{
	double r;

	r = servo->sample(servo, offset, local_ts, weight, state);

	switch (*state) {
	case SERVO_UNLOCKED:
		servo->curr_offset_values = servo->num_offset_values;
		break;
	case SERVO_JUMP:
		servo->curr_offset_values = servo->num_offset_values;
		servo->first_update = 0;
		break;
	case SERVO_LOCKED:
		if (check_offset_threshold(servo, offset)) {
			*state = SERVO_LOCKED_STABLE;
		}
		servo->first_update = 0;
		break;
	case SERVO_LOCKED_STABLE:
		/*
		 * This case will never occur since the only place
		 * SERVO_LOCKED_STABLE is set is in this switch/case block
		 * (case SERVO_LOCKED).
		 */
		break;
	}

	return r;
}

void servo_sync_interval(struct servo *servo, double interval)
{
	servo->sync_interval(servo, interval);
}

void servo_reset(struct servo *servo)
{
	servo->reset(servo);
}

double servo_rate_ratio(struct servo *servo)
{
	if (servo->rate_ratio)
		return servo->rate_ratio(servo);

	return 1.0;
}

void servo_leap(struct servo *servo, int leap)
{
	if (servo->leap)
		servo->leap(servo, leap);
}

int servo_offset_threshold(struct servo *servo)
{
	return servo->offset_threshold;
}

/* other process Wait for the creator process to finish initialization */
#define SHM_INIT_MAGIC 0x12345678
#define MAX_INIT_WAIT_MS 5000
#define INIT_POLL_INTERVAL_US 1000

/* Shared memory key for servo state */
#define SERVO_SHM_NAME "/ptp_servo_state"
#define SERVO_SHM_SIZE sizeof(struct servo_state_shm)

/**
 * Initialize shared memory for servo state
 */
struct servo_state_shm *servo_state_shm_create(void)
{
    int fd;
    struct servo_state_shm *shm;
    int created = 0;

    /* Open or create shared memory object */
    fd = shm_open(SERVO_SHM_NAME, O_RDWR, 0666);
    if (fd == -1) {
        if (errno == ENOENT) {
            /* Create a new shared memory object */
            fd = shm_open(SERVO_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd == -1) {
                if (errno == EEXIST) {
                    /* Race condition: another process created it, try to open */
                    fd = shm_open(SERVO_SHM_NAME, O_RDWR, 0666);
                    if (fd == -1) {
                        pr_err("Failed to open existing shared memory: %s", strerror(errno));
                        return NULL;
                    }
                } else {
                    pr_err("Failed to create shared memory: %s", strerror(errno));
                    return NULL;
                }
            } else {
                created = 1;
            }
        } else {
            pr_err("Failed to open shared memory: %s", strerror(errno));
            return NULL;
        }
    }

    /* Set the size of the shared memory object */
    if (created) {
        if (ftruncate(fd, SERVO_SHM_SIZE) == -1) {
            pr_err("Failed to set shared memory size: %s", strerror(errno));
            close(fd);
            return NULL;
        }
    }

    /* Map the shared memory object */
    shm = mmap(NULL, SERVO_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        pr_err("Failed to map shared memory: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    /* Initialize the shared memory if we created it */
    if (created) {
        memset(shm, 0, SERVO_SHM_SIZE);
        shm->init_complete = 0;  /* Mark as not ready yet */
        shm->initialized = 0;
        shm->state = SERVO_UNLOCKED;
        shm->domain_id = -1;
        
        /* Initialize the semaphore for process synchronization */
        if (sem_init(&shm->mutex, 1, 1) == -1) {
            pr_err("Failed to initialize servo state semaphore: %s", strerror(errno));
            munmap(shm, SERVO_SHM_SIZE);
            close(fd);
            return NULL;
        }
        
        /* Memory barrier to ensure all initialization is visible */
        __sync_synchronize();
        
        /* Mark shared memory as fully initialized and ready */
        shm->init_complete = 0x12345678;  /* Magic number */
        pr_debug("Servo state shared memory initialized by creator process");
    } else {  
        /* Wait for the creator process to finish initialization */
        int retry_count = 0;
             
        while (shm->init_complete != SHM_INIT_MAGIC && 
               retry_count < (MAX_INIT_WAIT_MS * 1000 / INIT_POLL_INTERVAL_US)) {
            usleep(INIT_POLL_INTERVAL_US);
            retry_count++;
        }
        
        if (shm->init_complete != SHM_INIT_MAGIC) {
            pr_err("Timeout waiting for servo state shared memory initialization");
            munmap(shm, SERVO_SHM_SIZE);
            close(fd);
            return NULL;
        }
        
        pr_debug("Servo state shared memory opened by secondary process (waited %d ms)", 
                 retry_count * INIT_POLL_INTERVAL_US / 1000);
    }

    close(fd);
    return shm;
}

/**
 * Detach from shared memory
 */
void servo_state_shm_destroy(struct servo_state_shm *shm)
{
    if (shm) {
        /* Destroy the semaphore */
        sem_destroy(&shm->mutex);
        munmap(shm, SERVO_SHM_SIZE);
    }
}

/**
 * Update the servo state in shared memory
 */
int servo_state_shm_update(struct servo_state_shm *shm, enum servo_state state, int domain_id)
{
    if (!shm) {
        return -1;
    }

    /* Acquire the semaphore lock */
    if (sem_wait(&shm->mutex) == -1) {
        pr_err("Failed to acquire servo state semaphore: %s", strerror(errno));
        return -1;
    }

    /* Critical section - update shared data */
    shm->state = state;
    shm->domain_id = domain_id;
    shm->initialized = 1;
    
    /* Release the semaphore lock */
    if (sem_post(&shm->mutex) == -1) {
        pr_err("Failed to release servo state semaphore: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

/**
 * Read the servo state from shared memory
 */
int servo_state_shm_read(struct servo_state_shm *shm, int domain_id, enum servo_state *state)
{
    if (!shm || !state) {
        return -1;
    }

    /* Acquire the semaphore lock */
    if (sem_wait(&shm->mutex) == -1) {
        pr_err("Failed to acquire servo state semaphore for read: %s", strerror(errno));
        return -1;
    }

    /* Critical section - read shared data */
    if (!shm->initialized || shm->domain_id != domain_id) {
        /* Release lock before returning */
        sem_post(&shm->mutex);
        return -1;
    }

    *state = shm->state;
    
    /* Release the semaphore lock */
    if (sem_post(&shm->mutex) == -1) {
        pr_err("Failed to release servo state semaphore after read: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

/**
 * Update the sync_received state in shared memory
 */
 int sync_state_shm_update(struct slave_servo_stable_shm *shm, int sync_received_state, int domain_id)
 {
     if (!shm) {
         return -1;
     }
 
     /* Acquire the semaphore lock */
     if (sem_wait(&shm->mutex) == -1) {
         pr_err("Failed to acquire sync state semaphore: %s", strerror(errno));
         return -1;
     }

     /* Critical section - update shared data */
     shm->sync_received = sync_received_state;
     shm->sync_domain_id = domain_id;
     pr_debug("*** Hotstandby-Domain%d-Master: write sync state(%d) to shm ***", domain_id, sync_received_state);
     
     /* Release the semaphore lock */
     if (sem_post(&shm->mutex) == -1) {
         pr_err("Failed to release sync state semaphore: %s", strerror(errno));
         return -1;
     }
     
     return 0;
 }

 /**
 * Read the sync_received state from shared memory
 */
int sync_state_shm_read(struct slave_servo_stable_shm *shm, int* sync_received_state)
{
    if (!shm || !sync_received_state) {
        pr_err("*** GM-Domain0-Master: read failed, shm=%p, sync_address=%p ***", shm, sync_received_state);
        return -1;
    }
    
    /* Acquire the semaphore lock */
    if (sem_wait(&shm->mutex) == -1) {
        pr_err("Failed to acquire sync state semaphore for read: %s", strerror(errno));
        return -1;
    }
    
    /* Critical section - read shared data */
    *sync_received_state = shm->sync_received;
    pr_debug("*** GM-Domain0-Master: shm_domain=%d, read sync state(%d) from shm ***", shm->sync_domain_id, *sync_received_state);
    
    /* Release the semaphore lock */
    if (sem_post(&shm->mutex) == -1) {
        pr_err("Failed to release sync state semaphore after read: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

/* Shared memory key for master restart detection */
#define MASTER_RESTART_SHM_NAME "/ptp_master_restart"
#define MASTER_RESTART_SHM_SIZE sizeof(struct master_restart_shm)

/**
 * Initialize shared memory for master restart detection
 */
struct master_restart_shm *master_restart_shm_create(void)
{
    int fd;
    struct master_restart_shm *shm;
    int created = 0;

    /* Open or create shared memory object */
    fd = shm_open(MASTER_RESTART_SHM_NAME, O_RDWR, 0666);
    if (fd == -1) {
        if (errno == ENOENT) {
            /* Create a new shared memory object */
            fd = shm_open(MASTER_RESTART_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd == -1) {
                if (errno == EEXIST) {
                    /* Race condition: another process created it, try to open */
                    fd = shm_open(MASTER_RESTART_SHM_NAME, O_RDWR, 0666);
                    if (fd == -1) {
                        pr_err("Failed to open existing master restart shared memory: %s", strerror(errno));
                        return NULL;
                    }
                } else {
                    pr_err("Failed to create master restart shared memory: %s", strerror(errno));
                    return NULL;
                }
            } else {
                created = 1;
            }
        } else {
            pr_err("Failed to open master restart shared memory: %s", strerror(errno));
            return NULL;
        }
    }

    /* Set the size of the shared memory object */
    if (created) {
        if (ftruncate(fd, MASTER_RESTART_SHM_SIZE) == -1) {
            pr_err("Failed to set master restart shared memory size: %s", strerror(errno));
            close(fd);
            return NULL;
        }
    }

    /* Map the shared memory object */
    shm = mmap(NULL, MASTER_RESTART_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        pr_err("Failed to map master restart shared memory: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    /* Initialize the shared memory if we created it */
    if (created) {
        memset(shm, 0, MASTER_RESTART_SHM_SIZE);
        shm->init_complete = 0;
        shm->initialized = 0;
        shm->master_restart_detected = 0;
        shm->domain_id = -1;
        
        /* Initialize the semaphore for process synchronization */
        if (sem_init(&shm->mutex, 1, 1) == -1) {
            pr_err("Failed to initialize master restart semaphore: %s", strerror(errno));
            munmap(shm, MASTER_RESTART_SHM_SIZE);
            close(fd);
            return NULL;
        }
        
        /* Memory barrier to ensure all initialization is visible */
        __sync_synchronize();
        
        /* Mark shared memory as fully initialized and ready */
        shm->init_complete = 0x12345678;
        pr_debug("Master restart shared memory initialized by creator process");
    } else {
        /* Wait for the creator process to finish initialization */
        int retry_count = 0;
              
        while (shm->init_complete != SHM_INIT_MAGIC && 
               retry_count < (MAX_INIT_WAIT_MS * 1000 / INIT_POLL_INTERVAL_US)) {
            usleep(INIT_POLL_INTERVAL_US);
            retry_count++;
        }
        
        if (shm->init_complete != SHM_INIT_MAGIC) {
            pr_err("Timeout waiting for master restart shared memory initialization");
            munmap(shm, MASTER_RESTART_SHM_SIZE);
            close(fd);
            return NULL;
        }
        
        pr_debug("Master restart shared memory opened by secondary process (waited %d ms)", 
                 retry_count * INIT_POLL_INTERVAL_US / 1000);
    }

    close(fd);
    return shm;
}

/**
 * Detach from master restart shared memory
 */
void master_restart_shm_destroy(struct master_restart_shm *shm)
{
    if (shm) {
        /* Destroy the semaphore */
        sem_destroy(&shm->mutex);
        munmap(shm, MASTER_RESTART_SHM_SIZE);
    }
}

/**
 * Update the master restart detection in shared memory
 */
int master_restart_shm_update(struct master_restart_shm *shm, int master_restart_detected, int domain_id)
{
    if (!shm) {
        return -1;
    }

    /* Acquire the semaphore lock */
    if (sem_wait(&shm->mutex) == -1) {
        pr_err("Failed to acquire master restart semaphore: %s", strerror(errno));
        return -1;
    }

    /* Critical section - update shared data */
    shm->master_restart_detected = master_restart_detected;
    shm->domain_id = domain_id;
    shm->initialized = 1;
    
    /* Release the semaphore lock */
    if (sem_post(&shm->mutex) == -1) {
        pr_err("Failed to release master restart semaphore: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

/**
 * Read the master restart detection from shared memory
 */
int master_restart_shm_read(struct master_restart_shm *shm, int domain_id, int *master_restart_detected)
{
    if (!shm || !master_restart_detected) {
        return -1;
    }
    
    /* Acquire the semaphore lock */
    if (sem_wait(&shm->mutex) == -1) {
        pr_err("Failed to acquire master restart semaphore for read: %s", strerror(errno));
        return -1;
    }
    
    /* Critical section - read shared data */
    if (!shm->initialized || shm->domain_id != domain_id) {
        /* Release lock before returning */
        sem_post(&shm->mutex);
        return -1;
    }
    
    *master_restart_detected = shm->master_restart_detected;
    
    /* Release the semaphore lock */
    if (sem_post(&shm->mutex) == -1) {
        pr_err("Failed to release master restart semaphore after read: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

/* Shared memory key for slave servo stable state */
#define SLAVE_SERVO_STABLE_SHM_NAME "/ptp_slave_servo_stable"
#define SLAVE_SERVO_STABLE_SHM_SIZE sizeof(struct slave_servo_stable_shm)

/**
 * Initialize shared memory for slave servo stable state
 */
struct slave_servo_stable_shm *slave_servo_stable_shm_create(void)
{
    int fd;
    struct slave_servo_stable_shm *shm;
    int created = 0;

    /* Open or create shared memory object */
    fd = shm_open(SLAVE_SERVO_STABLE_SHM_NAME, O_RDWR, 0666);
    if (fd == -1) {
        if (errno == ENOENT) {
            /* Create a new shared memory object */
            fd = shm_open(SLAVE_SERVO_STABLE_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd == -1) {
                if (errno == EEXIST) {
                    /* Race condition: another process created it, try to open */
                    fd = shm_open(SLAVE_SERVO_STABLE_SHM_NAME, O_RDWR, 0666);
                    if (fd == -1) {
                        pr_err("Failed to open existing slave servo stable shared memory: %s", strerror(errno));
                        return NULL;
                    }
                } else {
                    pr_err("Failed to create slave servo stable shared memory: %s", strerror(errno));
                    return NULL;
                }
            } else {
                created = 1;
            }
        } else {
            pr_err("Failed to open slave servo stable shared memory: %s", strerror(errno));
            return NULL;
        }
    }

    /* Set the size of the shared memory object */
    if (created) {
        if (ftruncate(fd, SLAVE_SERVO_STABLE_SHM_SIZE) == -1) {
            pr_err("Failed to set slave servo stable shared memory size: %s", strerror(errno));
            close(fd);
            return NULL;
        }
    }

    /* Map the shared memory object */
    shm = mmap(NULL, SLAVE_SERVO_STABLE_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        pr_err("Failed to map slave servo stable shared memory: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    /* Initialize the shared memory if we created it */
    if (created) {
        memset(shm, 0, SLAVE_SERVO_STABLE_SHM_SIZE);
        shm->init_complete = 0;
        shm->initialized = 0;
        shm->slave_servo_stable = 0;
        shm->domain_id = -1;
        
        shm->sync_received = 0;
        shm->sync_domain_id = -1;
        
        /* Initialize the semaphore for process synchronization */
        if (sem_init(&shm->mutex, 1, 1) == -1) {
            pr_err("Failed to initialize slave servo stable semaphore: %s", strerror(errno));
            munmap(shm, SLAVE_SERVO_STABLE_SHM_SIZE);
            close(fd);
            return NULL;
        }
        
        /* Memory barrier to ensure all initialization is visible */
        __sync_synchronize();
        
        /* Mark shared memory as fully initialized and ready */
        shm->init_complete = 0x12345678;
        pr_debug("Slave servo stable shared memory initialized by creator process");
    } else {
        /* Wait for the creator process to finish initialization */
        int retry_count = 0;
        
        while (shm->init_complete != SHM_INIT_MAGIC && 
               retry_count < (MAX_INIT_WAIT_MS * 1000 / INIT_POLL_INTERVAL_US)) {
            usleep(INIT_POLL_INTERVAL_US);
            retry_count++;
        }
        
        if (shm->init_complete != SHM_INIT_MAGIC) {
            pr_err("Timeout waiting for slave servo stable shared memory initialization");
            munmap(shm, SLAVE_SERVO_STABLE_SHM_SIZE);
            close(fd);
            return NULL;
        }
        
        pr_debug("Slave servo stable shared memory opened by secondary process (waited %d ms)", 
                 retry_count * INIT_POLL_INTERVAL_US / 1000);
    }

    close(fd);
    return shm;
}

/**
 * Detach from slave servo stable shared memory
 */
void slave_servo_stable_shm_destroy(struct slave_servo_stable_shm *shm)
{
    if (shm) {
        /* Destroy the semaphore */
        sem_destroy(&shm->mutex);
        munmap(shm, SLAVE_SERVO_STABLE_SHM_SIZE);
    }
}

/**
 * Update the slave servo stable state in shared memory
 */
int slave_servo_stable_shm_update(struct slave_servo_stable_shm *shm, int slave_servo_stable, int domain_id)
{
    if (!shm) {
        return -1;
    }

    /* Acquire the semaphore lock */
    if (sem_wait(&shm->mutex) == -1) {
        pr_err("Failed to acquire slave servo stable semaphore: %s", strerror(errno));
        return -1;
    }

    /* Critical section - update shared data */
    shm->slave_servo_stable = slave_servo_stable;
    shm->domain_id = domain_id;
    shm->initialized = 1;
    
    /* Release the semaphore lock */
    if (sem_post(&shm->mutex) == -1) {
        pr_err("Failed to release slave servo stable semaphore: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

/**
 * Read the slave servo stable state from shared memory
 */
int slave_servo_stable_shm_read(struct slave_servo_stable_shm *shm, int domain_id, int *slave_servo_stable)
{
    if (!shm || !slave_servo_stable) {
        return -1;
    }
    
    /* Acquire the semaphore lock */
    if (sem_wait(&shm->mutex) == -1) {
        pr_err("Failed to acquire slave servo stable semaphore for read: %s", strerror(errno));
        return -1;
    }
    
    /* Critical section - read shared data */
    if (!shm->initialized || shm->domain_id != domain_id) {
        /* Release lock before returning */
        sem_post(&shm->mutex);
        return -1;
    }
    
    *slave_servo_stable = shm->slave_servo_stable;
    
    /* Release the semaphore lock */
    if (sem_post(&shm->mutex) == -1) {
        pr_err("Failed to release slave servo stable semaphore after read: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}