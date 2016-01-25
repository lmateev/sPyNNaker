
# front end common imports
from pacman.utilities.utility_objs.progress_bar import ProgressBar
from spinn_front_end_common.abstract_models.\
    abstract_data_specable_vertex import \
    AbstractDataSpecableVertex
from spinn_front_end_common.utilities import exceptions as common_exceptions

# spynnaker imports
from spynnaker.pyNN.models.abstract_models.\
    abstract_has_first_machine_time_step import \
    AbstractHasFirstMachineTimeStep
from spynnaker.pyNN.models.common.abstract_gsyn_recordable import \
    AbstractGSynRecordable
from spynnaker.pyNN.models.common.abstract_spike_recordable import \
    AbstractSpikeRecordable
from spynnaker.pyNN.models.common.abstract_v_recordable import \
    AbstractVRecordable

# general imports
import math
import logging

logger = logging.getLogger(__name__)


class SpyNNakerRuntimeUpdator(object):
    """ updates the commuilative ran time
    """

    def __call__(self, iteration, steps, total_run_time_executed_so_far,
                 partitionable_graph):

        progress_bar = ProgressBar(len(partitionable_graph.vertices),
                                   "Updating python vertices runtime")

        # deduce the new runtime position
        set_runtime = total_run_time_executed_so_far
        set_runtime += steps[iteration]

        # update the partitionable vertices
        for vertex in partitionable_graph.vertices:
            vertex.set_no_machine_time_steps(set_runtime)
            progress_bar.update()
        progress_bar.end()

        # calculate number of machine time steps
        for vertex in partitionable_graph.vertices:
            if isinstance(vertex, AbstractHasFirstMachineTimeStep):
                vertex.set_first_machine_time_step(
                    total_run_time_executed_so_far)
            progress_bar.update()
        progress_bar.end()

        return {'total_run_time': set_runtime}

    @staticmethod
    def _calculate_number_of_machine_time_steps(
            next_run_time, current_run_ms, machine_time_step,
            partitionable_graph):

        total_run_time = next_run_time
        if next_run_time is not None:
            total_run_time += current_run_ms
            machine_time_steps = (
                (total_run_time * 1000.0) / machine_time_step)
            if machine_time_steps != int(machine_time_steps):
                logger.warn(
                    "The runtime and machine time step combination result in "
                    "a fractional number of machine time steps")
            no_machine_time_steps = int(math.ceil(machine_time_steps))
        else:
            no_machine_time_steps = None

        for vertex in partitionable_graph.vertices:
            if isinstance(vertex, AbstractDataSpecableVertex):
                vertex.set_no_machine_time_steps(no_machine_time_steps)
        return total_run_time
