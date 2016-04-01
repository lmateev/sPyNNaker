/*! \file
 *
 *  \brief This file contains the main functions for a poisson spike generator.
 *
 *
 */

#include "../../common/out_spikes.h"
#include "../../common/maths-util.h"

#include <data_specification.h>
#include <recording.h>
#include <debug.h>
#include <random.h>
#include <simulation.h>
#include <spin1_api.h>
#include <string.h>

//! data structure for spikes which have multiple timer tick between firings
//! this is separated from spikes which fire at least once every timer tick as
//! there are separate algorithms for each type.
typedef struct slow_spike_source_t {
    uint32_t neuron_id;
    uint32_t start_ticks;
    uint32_t end_ticks;

    REAL mean_isi_ticks;
    REAL time_to_spike_ticks;
} slow_spike_source_t;

//! data structure for spikes which have at least one spike fired per timer
//! tick; this is separated from spikes which have multiple timer ticks
//! between firings as there are separate algorithms for each type.
typedef struct fast_spike_source_t {
    uint32_t neuron_id;
    uint32_t start_ticks;
    uint32_t end_ticks;
    UFRACT exp_minus_lambda;
} fast_spike_source_t;

//! spike source array region ids in human readable form
typedef enum region {
    SYSTEM, POISSON_PARAMS,
    BUFFERING_OUT_SPIKE_RECORDING_REGION,
    BUFFERING_OUT_CONTROL_REGION,
    PROVENANCE_REGION
} region;

#define NUMBER_OF_REGIONS_TO_RECORD 1

typedef enum callback_priorities{
    SDP = 0, TIMER_2 = 1, TIMER = 2
} callback_priorities;

//! what each position in the poisson parameter region actually represent in
//! terms of data (each is a word)
typedef enum poisson_region_parameters{
    HAS_KEY, TRANSMISSION_KEY, RANDOM_BACKOFF, TIMER_2_TICK,
    PARAMETER_SEED_START_POSITION,
} poisson_region_parameters;

// Globals
//! global variable which contains all the data for neurons which are expected
//! to exhibit slow spike generation (less than 1 per timer tick)
//! (separated for efficiently purposes)
static slow_spike_source_t *slow_spike_source_array = NULL;

//! global variable which contains all the data for neurons which are expected
//! to exhibit fast spike generation (more than than 1 per timer tick)
//! (separated for efficiently purposes)
static fast_spike_source_t *fast_spike_source_array = NULL;

//! counter for how many neurons exhibit slow spike generation
static uint32_t num_slow_spike_sources = 0;

//! counter for how many neurons exhibit fast spike generation
static uint32_t num_fast_spike_sources = 0;

//! a variable that will contain the seed to initiate the poisson generator.
static mars_kiss64_seed_t spike_source_seed;

//! a variable which checks if there has been a key allocated to this spike
//! source Poisson
static bool has_been_given_key;

//! A variable that contains the key value that this model should transmit with
static uint32_t key;

//! An amount of microseconds to back off before starting the timer, in an
//! attempt to avoid overloading the network
static uint32_t random_backoff_us;

//! keeps track of which types of recording should be done to this model.
static uint32_t recording_flags = 0;

//! the time interval parameter TODO this variable could be removed and use the
//! timer tick callback timer value.
static uint32_t time;

//! the next index to run the generation routines on
static uint32_t source_index;

//! Determines whether the source timer is on slow or fast sources
static bool on_slow_sources = false;

//! The current position in the slow spike source array
static slow_spike_source_t *next_slow_spike_source;

//! The current position in the fast spike source array
static fast_spike_source_t *next_fast_spike_source;

//! Indicates whether the timer for the sources is currently running or not
static bool timer_running = false;

//! the number of timer ticks that this model should run for before exiting.
static uint32_t simulation_ticks = 0;

//! the int that represents the bool for if the run is infinite or not.
static uint32_t infinite_run;

static uint32_t n_timer1_reent = 0;
static uint32_t n_timer2_reent = 0;

//! \brief deduces the time in timer ticks until the next spike is to occur
//!        given the mean inter-spike interval
//! \param[in] mean_inter_spike_interval_in_ticks The mean number of ticks
//!            before a spike is expected to occur in a slow process.
//! \return a real which represents time in timer ticks until the next spike is
//!         to occur
static inline REAL slow_spike_source_get_time_to_spike(
        REAL mean_inter_spike_interval_in_ticks) {
    return exponential_dist_variate(mars_kiss64_seed, spike_source_seed)
            * mean_inter_spike_interval_in_ticks;
}

//! \brief Determines how many spikes to transmit this timer tick.
//! \param[in] exp_minus_lambda The amount of spikes expected to be produced
//!            this timer interval (timer tick in real time)
//! \return a uint32_t which represents the number of spikes to transmit
//!         this timer tick
static inline uint32_t fast_spike_source_get_num_spikes(
        UFRACT exp_minus_lambda) {
    if (bitsulr(exp_minus_lambda) == bitsulr(UFRACT_CONST(0.0))) {
        return 0;
    }
    else {
        return poisson_dist_variate_exp_minus_lambda(
            mars_kiss64_seed, spike_source_seed, exp_minus_lambda);
    }
}

//! \entry method for reading the parameters stored in Poisson parameter region
//! \param[in] address the absolute SDRAm memory address to which the
//!            Poisson parameter region starts.
//! \return a boolean which is True if the parameters were read successfully or
//!         False otherwise
bool read_poisson_parameters(address_t address, uint32_t *timer_2_period) {

    log_info("read_parameters: starting");

    has_been_given_key = address[HAS_KEY];
    key = address[TRANSMISSION_KEY];
    random_backoff_us = address[RANDOM_BACKOFF];
    *timer_2_period = address[TIMER_2_TICK];
    log_info(
        "\tkey = %08x, backoff = %u, timer_2 = %u", key, random_backoff_us,
        *timer_2_period);

    uint32_t seed_size = sizeof(mars_kiss64_seed_t) / sizeof(uint32_t);
    memcpy(spike_source_seed, &address[PARAMETER_SEED_START_POSITION],
        seed_size * sizeof(uint32_t));
    validate_mars_kiss64_seed(spike_source_seed);

    log_info("\tSeed (%u) = %u %u %u %u", seed_size, spike_source_seed[0],
             spike_source_seed[1], spike_source_seed[2], spike_source_seed[3]);

    num_slow_spike_sources = address[PARAMETER_SEED_START_POSITION + seed_size];
    num_fast_spike_sources = address[PARAMETER_SEED_START_POSITION +
                                     seed_size + 1];
    log_info("\t slow spike sources = %u, fast spike sources = %u,",
             num_slow_spike_sources, num_fast_spike_sources);

    // Allocate DTCM for array of slow spike sources and copy block of data
    if (num_slow_spike_sources > 0) {
        slow_spike_source_array = (slow_spike_source_t*) spin1_malloc(
            num_slow_spike_sources * sizeof(slow_spike_source_t));
        if (slow_spike_source_array == NULL) {
            log_error("Failed to allocate slow_spike_source_array");
            return false;
        }
        uint32_t slow_spikes_offset = PARAMETER_SEED_START_POSITION +
                                    seed_size + 2;
        memcpy(slow_spike_source_array,
                &address[slow_spikes_offset],
               num_slow_spike_sources * sizeof(slow_spike_source_t));

        // Loop through slow spike sources and initialise 1st time to spike
        for (index_t s = 0; s < num_slow_spike_sources; s++) {
            slow_spike_source_array[s].time_to_spike_ticks =
                slow_spike_source_get_time_to_spike(
                    slow_spike_source_array[s].mean_isi_ticks);
        }
    }

    // Allocate DTCM for array of fast spike sources and copy block of data
    if (num_fast_spike_sources > 0) {
        fast_spike_source_array = (fast_spike_source_t*) spin1_malloc(
            num_fast_spike_sources * sizeof(fast_spike_source_t));
        if (fast_spike_source_array == NULL) {
            log_error("Failed to allocate fast_spike_source_array");
            return false;
        }
        // locate offset for the fast spike sources in the SDRAM from where the
        // seed finished.
        uint32_t fast_spike_source_offset =
                PARAMETER_SEED_START_POSITION + seed_size + 2 +
            + (num_slow_spike_sources * (sizeof(slow_spike_source_t)
                / sizeof(uint32_t)));
        memcpy(fast_spike_source_array, &address[fast_spike_source_offset],
               num_fast_spike_sources * sizeof(fast_spike_source_t));

        for (index_t s = 0; s < num_fast_spike_sources; s++) {
            log_debug("\t\tNeuron id %d, exp(-k) = %0.8x",
                      fast_spike_source_array[s].neuron_id,
                      fast_spike_source_array[s].exp_minus_lambda);
        }
    }
    log_info("read_parameters: completed successfully");
    return true;
}

//! \brief Initialises the recording parts of the model
//! \return True if recording initialisation is successful, false otherwise
static bool initialise_recording(){

    // Get the address this core's DTCM data starts at from SRAM
    address_t address = data_specification_get_data_address();

    // Get the system region
    address_t system_region = data_specification_get_region(
            SYSTEM, address);

    // Get the recording information
    uint8_t regions_to_record[] = {
        BUFFERING_OUT_SPIKE_RECORDING_REGION,
    };
    uint8_t n_regions_to_record = NUMBER_OF_REGIONS_TO_RECORD;
    uint32_t *recording_flags_from_system_conf =
        &system_region[SIMULATION_N_TIMING_DETAIL_WORDS];
    uint8_t state_region = BUFFERING_OUT_CONTROL_REGION;

    bool success = recording_initialize(
        n_regions_to_record, regions_to_record,
        recording_flags_from_system_conf, state_region, 2, &recording_flags);
    log_info("Recording flags = 0x%08x", recording_flags);
    return success;
}

//! Initialises the model by reading in the regions and checking recording
//! data.
//! \param[in] *timer_period a pointer for the memory address where the timer
//!            period should be stored during the function.
//! \return boolean of True if it successfully read all the regions and set up
//!         all its internal data structures. Otherwise returns False
static bool initialize(uint32_t *timer_period, uint32_t *timer_2_period) {
    log_info("Initialise: started");

    // Get the address this core's DTCM data starts at from SRAM
    address_t address = data_specification_get_data_address();

    // Read the header
    if (!data_specification_read_header(address)) {
        return false;
    }

    // Get the timing details
    address_t system_region = data_specification_get_region(
            SYSTEM, address);
    if (!simulation_read_timing_details(
            system_region, APPLICATION_NAME_HASH, timer_period)) {
        return false;
    }

    // setup recording region
    if (!initialise_recording()){
        return false;
    }

    // Setup regions that specify spike source array data
    if (!read_poisson_parameters(
            data_specification_get_region(POISSON_PARAMS, address),
            timer_2_period)) {
        return false;
    }

    log_info("Initialise: completed successfully");

    return true;
}

void resume_callback() {

    // handle resetting the recording state
    // Get the recording information
    address_t address = data_specification_get_data_address();
    address_t system_region = data_specification_get_region(
        SYSTEM, address);
    uint8_t regions_to_record[] = {
        BUFFERING_OUT_SPIKE_RECORDING_REGION,
    };
    uint8_t n_regions_to_record = NUMBER_OF_REGIONS_TO_RECORD;
    uint32_t *recording_flags_from_system_conf =
        &system_region[SIMULATION_N_TIMING_DETAIL_WORDS];
    uint8_t state_region = BUFFERING_OUT_CONTROL_REGION;

    recording_initialize(
        n_regions_to_record, regions_to_record,
        recording_flags_from_system_conf, state_region, 2,
        &recording_flags);
}


// Called to finish processing the sources
void _finish_sources() {
    spin1_disable_timer_2();
    timer_running = false;

    // Record output spikes if required
    if (recording_flags > 0) {
        out_spikes_record(0, time);
    }
    out_spikes_reset();

    if (recording_flags > 0) {
        recording_do_timestep_update(time);
    }
}


//! \brief Second timer interrupt callback
//! \param[in] timer_count the number of times this call back has been
//!            executed since start of simulation
//! \param[in] unused for consistency sake of the API always returning two
//!            parameters, this parameter has no semantics currently and thus
//!            is set to 0
//! \return None
void timer2_callback(uint timer_count, uint unused) {
    use(timer_count);
    use(unused);

    // If this is an accident, skip it
    if (!timer_running) {
        n_timer2_reent += 1;
        return;
    }

    // Loop through slow spike sources
    if (on_slow_sources) {

        // If this spike source is active this tick
        slow_spike_source_t *slow_spike_source = next_slow_spike_source++;
        if ((time >= slow_spike_source->start_ticks)
                && (time < slow_spike_source->end_ticks)
                && (REAL_COMPARE(slow_spike_source->mean_isi_ticks, !=,
                    REAL_CONST(0.0)))) {

            // If this spike source should spike now
            if (REAL_COMPARE(slow_spike_source->time_to_spike_ticks, <=,
                             REAL_CONST(0.0))) {

                // Write spike to out spikes
                out_spikes_set_spike(slow_spike_source->neuron_id);

                // if no key has been given, do not send spike to fabric.
                if (has_been_given_key) {

                    // Send package
                    while (!spin1_send_mc_packet(
                            key | slow_spike_source->neuron_id, 0,
                            NO_PAYLOAD)) {
                        spin1_delay_us(1);
                    }
                    log_debug("Sending spike packet %x at %d\n",
                        key | slow_spike_source->neuron_id, time);
                }

                // Update time to spike
                slow_spike_source->time_to_spike_ticks +=
                    slow_spike_source_get_time_to_spike(
                        slow_spike_source->mean_isi_ticks);
            }

            // Subtract tick
            slow_spike_source->time_to_spike_ticks -= REAL_CONST(1.0);
        }

        source_index -= 1;
        if (source_index == 0) {
            on_slow_sources = false;
            if (num_fast_spike_sources > 0) {
                source_index = num_fast_spike_sources;
            } else {
                _finish_sources();
            }
        }
    } else {

        fast_spike_source_t *fast_spike_source = next_fast_spike_source++;
        if (time >= fast_spike_source->start_ticks
                && time < fast_spike_source->end_ticks) {

            // Get number of spikes to send this tick
            uint32_t num_spikes = fast_spike_source_get_num_spikes(
                fast_spike_source->exp_minus_lambda);
            log_debug("Generating %d spikes", num_spikes);

            // If there are any
            if (num_spikes > 0) {

                // Write spike to out spikes
                out_spikes_set_spike(fast_spike_source->neuron_id);

                // Send spikes
                const uint32_t spike_key = key | fast_spike_source->neuron_id;
                for (uint32_t s = num_spikes; s > 0; s--) {

                    // if no key has been given, do not send spike to fabric.
                    if (has_been_given_key){
                        log_debug("Sending spike packet %x at %d\n",
                                  spike_key, time);
                        while (!spin1_send_mc_packet(spike_key, 0,
                                                     NO_PAYLOAD)) {
                            spin1_delay_us(1);
                        }
                    }
                }
            }
        }

        source_index -= 1;
        if (source_index == 0) {
            _finish_sources();
        }
    }
}

//! \brief Timer interrupt callback
//! \param[in] timer_count the number of times this call back has been
//!            executed since start of simulation
//! \param[in] unused for consistency sake of the API always returning two
//!            parameters, this parameter has no semantics currently and thus
//!            is set to 0
//! \return None
void timer_callback(uint timer_count, uint unused) {
    use(timer_count);
    use(unused);

    // If this has come too early, skip it
    if (timer_running) {
        n_timer1_reent += 1;
        return;
    }

    time++;

    log_debug("Timer tick %u", time);

    // If a fixed number of simulation ticks are specified and these have passed
    if (infinite_run != TRUE && time >= simulation_ticks) {

        // Finalise any recordings that are in progress, writing back the final
        // amounts of samples recorded to SDRAM
        if (recording_flags > 0) {
            recording_finalise();
        }
        // go into pause and resume state
        log_info("Timer 1 Reentered %u times", n_timer1_reent);
        log_info("Timer 2 Reentered %u times", n_timer2_reent);
        simulation_handle_pause_resume(resume_callback);
    } else {
        timer_running = true;
        if (num_slow_spike_sources > 0) {
            on_slow_sources = true;
            source_index = num_slow_spike_sources;
        } else {
            on_slow_sources = false;
            source_index = num_fast_spike_sources;
        }
        next_slow_spike_source = slow_spike_source_array;
        next_fast_spike_source = fast_spike_source_array;
        spin1_delay_us(random_backoff_us);
        spin1_enable_timer_2();
    }
}

//! The entry point for this model
void c_main(void) {

    // Load DTCM data
    uint32_t timer_period;
    uint32_t timer_2_period;
    if (!initialize(&timer_period, &timer_2_period)) {
        log_error("Error in initialisation - exiting!");
        rt_error(RTE_SWERR);
    }

    // Start the time at "-1" so that the first tick will be 0
    time = UINT32_MAX;

    // Initialise out spikes buffer to support number of neurons
    if (!out_spikes_initialize(num_fast_spike_sources
                               + num_slow_spike_sources)) {
         rt_error(RTE_SWERR);
    }

    // Set timer tick (in microseconds)
    spin1_set_timer_tick(timer_period);
    spin1_set_timer_2_tick(timer_2_period);

    // Register callback
    spin1_callback_on(TIMER_TICK, timer_callback, TIMER);
    spin1_callback_on(TIMER_TICK_2, timer2_callback, TIMER_2);

    // Set up callback listening to SDP messages
    simulation_register_simulation_sdp_callback(
        &simulation_ticks, &infinite_run, SDP);

    // set up provenance registration
    simulation_register_provenance_callback(NULL, PROVENANCE_REGION);

    simulation_run();
}
