/**
 * @file servo.h
 * @brief Implements a generic clock servo interface.
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
#ifndef HAVE_SERVO_H
#define HAVE_SERVO_H

#include <stdint.h>
#include <semaphore.h>

struct config;

/** Opaque type */
struct servo;

/**
 * Defines the available servo cores
 */
enum servo_type {
	CLOCK_SERVO_PI,
	CLOCK_SERVO_LINREG,
	CLOCK_SERVO_NTPSHM,
	CLOCK_SERVO_NULLF,
	CLOCK_SERVO_REFCLOCK_SOCK,
};

/**
 * Defines the caller visible states of a clock servo.
 */
enum servo_state {

	/**
	 * The servo is not yet ready to track the master clock.
	 */
	SERVO_UNLOCKED,

	/**
	 * The servo is ready to track and requests a clock jump to
	 * immediately correct the estimated offset.
	 */
	SERVO_JUMP,

	/**
	 * The servo is tracking the master clock.
	 */
	SERVO_LOCKED,

	/**
	 * The Servo has stabilized. The last 'servo_num_offset_values' values
	 * of the estimated threshold are less than servo_offset_threshold.
	 */
	SERVO_LOCKED_STABLE,
};

/**
 * Structure for sharing servo state between domains
 */
struct servo_state_shm {
    int init_complete;       /* Magic value (0x12345678) indicating structure is fully initialized and ready */
    enum servo_state state;  /* Current servo state of domain 0 */
    int initialized;         /* Whether state has been initialized */
    int domain_id;           /* Domain ID that wrote the state */
    sem_t mutex;             /* Semaphore for thread/process synchronization */
};

/**
 * Structure for sharing master restart detection between domains
 */
struct master_restart_shm {
    int init_complete;            /* Magic value (0x12345678) indicating structure is fully initialized and ready */
    int master_restart_detected;  /* Master restart detection flag */
    int initialized;              /* Whether state has been initialized */
    int domain_id;                /* Domain ID that wrote the state */
    sem_t mutex;                  /* Semaphore for thread/process synchronization */
};

/**
 * Structure for sharing slave servo stable state between domains
 */
struct slave_servo_stable_shm {
    int init_complete;            /* Magic value (0x12345678) indicating structure is fully initialized and ready */
    int slave_servo_stable;       /* Slave servo stable state flag */
    int initialized;              /* Whether state has been initialized */
    int domain_id;                /* Domain ID that wrote the state */
    
    int sync_received;            /* Whether domain 1's sync has been received */
    int sync_domain_id;           /* Domain ID that wrote the sync state */
    sem_t mutex;                  /* Semaphore for thread/process synchronization */
};

/**
 * Initialize shared memory for servo state
 * @return Pointer to shared memory region, or NULL on error
 */
struct servo_state_shm *servo_state_shm_create(void);

/**
 * Detach from shared memory
 * @param shm Pointer to shared memory obtained via @ref servo_state_shm_create()
 */
void servo_state_shm_destroy(struct servo_state_shm *shm);

/**
 * Update the servo state in shared memory
 * @param shm Pointer to shared memory
 * @param state The servo state to store
 * @param domain_id The domain ID writing the state
 * @return 0 on success, -1 on error
 */
int servo_state_shm_update(struct servo_state_shm *shm, enum servo_state state, int domain_id);

/**
 * Read the servo state from shared memory
 * @param shm Pointer to shared memory
 * @param domain_id The domain ID to check against (typically 0)
 * @param state Pointer to store the read state
 * @return 0 on success, -1 if not initialized or wrong domain
 */
int servo_state_shm_read(struct servo_state_shm *shm, int domain_id, enum servo_state *state);

/**
 * Update the sync_received state in shared memory
 * @param shm Pointer to shared memory
 * @param sync_received_state The sync_received state to store
 * @param domain_id The domain ID writing the state
 * @return 0 on success, -1 on error
 */
int sync_state_shm_update(struct slave_servo_stable_shm *shm, int sync_received_state, int domain_id);

/**
 * Read the sync_received state from shared memory
 * @param shm Pointer to shared memory
 * @param sync_received_state Pointer to store the read state
 * @return 0 on success, -1 if not initialized or wrong domain
 */
int sync_state_shm_read(struct slave_servo_stable_shm *shm, int *sync_received_state);

/**
 * Initialize shared memory for master restart detection
 * @return Pointer to shared memory region, or NULL on error
 */
struct master_restart_shm *master_restart_shm_create(void);

/**
 * Detach from master restart shared memory
 * @param shm Pointer to shared memory obtained via @ref master_restart_shm_create()
 */
void master_restart_shm_destroy(struct master_restart_shm *shm);

/**
 * Update the master restart detection in shared memory
 * @param shm Pointer to shared memory
 * @param master_restart_detected The master restart flag to store
 * @param domain_id The domain ID writing the state
 * @return 0 on success, -1 on error
 */
int master_restart_shm_update(struct master_restart_shm *shm, int master_restart_detected, int domain_id);

/**
 * Read the master restart detection from shared memory
 * @param shm Pointer to shared memory
 * @param domain_id The domain ID to check against (typically 0)
 * @param master_restart_detected Pointer to store the read flag
 * @return 0 on success, -1 if not initialized or wrong domain
 */
int master_restart_shm_read(struct master_restart_shm *shm, int domain_id, int *master_restart_detected);

/**
 * Initialize shared memory for slave servo stable state
 * @return Pointer to shared memory region, or NULL on error
 */
struct slave_servo_stable_shm *slave_servo_stable_shm_create(void);

/**
 * Detach from slave servo stable shared memory
 * @param shm Pointer to shared memory obtained via @ref slave_servo_stable_shm_create()
 */
void slave_servo_stable_shm_destroy(struct slave_servo_stable_shm *shm);

/**
 * Update the slave servo stable state in shared memory
 * @param shm Pointer to shared memory
 * @param slave_servo_stable The slave servo stable flag to store
 * @param domain_id The domain ID writing the state
 * @return 0 on success, -1 on error
 */
int slave_servo_stable_shm_update(struct slave_servo_stable_shm *shm, int slave_servo_stable, int domain_id);

/**
 * Read the slave servo stable state from shared memory
 * @param shm Pointer to shared memory
 * @param domain_id The domain ID to check against (typically 0)
 * @param slave_servo_stable Pointer to store the read flag
 * @return 0 on success, -1 if not initialized or wrong domain
 */
int slave_servo_stable_shm_read(struct slave_servo_stable_shm *shm, int domain_id, int *slave_servo_stable);

/**
 * Create a new instance of a clock servo.
 * @param type    The type of the servo to create.
 * @param fadj    The clock's current adjustment in parts per billion.
 * @param max_ppb The absolute maxinum adjustment allowed by the clock
 *                in parts per billion. The clock servo will clamp its
 *                output according to this limit.
 * @param sw_ts   Indicates that software time stamping will be used,
 *                and the servo should use more aggressive filtering.
 * @return A pointer to a new servo on success, NULL otherwise.
 */
struct servo *servo_create(struct config *cfg, enum servo_type type,
			   double fadj, int max_ppb, int sw_ts);

/**
 * Destroy an instance of a clock servo.
 * @param servo Pointer to a servo obtained via @ref servo_create().
 */
void servo_destroy(struct servo *servo);

/**
 * Feed a sample into a clock servo.
 * @param servo     Pointer to a servo obtained via @ref servo_create().
 * @param offset    The estimated clock offset in nanoseconds.
 * @param local_ts  The local time stamp of the sample in nanoseconds.
 * @param weight    The weight of the sample, larger if more reliable,
 *                  1.0 is the maximum value.
 * @param state     Returns the servo's state.
 * @return The clock adjustment in parts per billion.
 */
double servo_sample(struct servo *servo,
		    int64_t offset,
		    uint64_t local_ts,
		    double weight,
		    enum servo_state *state);

/**
 * Inform a clock servo about the master's sync interval.
 * @param servo   Pointer to a servo obtained via @ref servo_create().
 * @param interval The sync interval in seconds.
 */
void servo_sync_interval(struct servo *servo, double interval);

/**
 * Reset a clock servo.
 * @param servo   Pointer to a servo obtained via @ref servo_create().
 */
void servo_reset(struct servo *servo);

/**
 * Obtain ratio between master's frequency and current servo frequency.
 * @param servo   Pointer to a servo obtained via @ref servo_create().
 * @return   The rate ratio, 1.0 is returned when not known.
 */
double servo_rate_ratio(struct servo *servo);

/**
 * Inform a clock servo about upcoming leap second.
 * @param servo   Pointer to a servo obtained via @ref servo_create().
 * @param leap    +1 when leap second will be inserted, -1 when leap second
 *                will be deleted, 0 when it passed.
 */
void servo_leap(struct servo *servo, int leap);

/**
 * Get the offset threshold for triggering the interval change request.
 * @param servo   Pointer to a servo obtained via @ref servo_create().
 * @return        The offset threshold set by the user.
 */
int servo_offset_threshold(struct servo *servo);

#endif