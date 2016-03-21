class PopulationView(object):
    def __init__(self, parent, selector, label):
        pass

    def __add__(self, other):
        pass

    def __getitem__(self, index):
        pass

    def __iter__(self):
        pass

    def __len__(self):
        pass

    def all(self):
        pass

    def can_record(self, variable):
        pass

    def describe(self, template='populationview_default.txt', engine='default'):
        pass

    def get(self, parameter_name, gather=False):
        pass

    def getSpikes(self, gather=True, compatible_output=True):
        pass

    def get_gsyn(self, gather=True, compatible_output=True):
        pass

    def get_spike_counts(self, gather=True):
        pass

    def get_v(self, gather=True, compatible_output=True):
        pass

    def id_to_index(self, id):
        pass

    def initialize(self, variable, value):
        pass

    def inject(self, current_source):
        pass

    def is_local(self, id):
        pass

    def meanSpikeCount(self, gather=True):
        pass

    def nearest(self, position):
        pass

    def printSpikes(self, file, gather=True, compatible_output=True):
        pass

    def print_gsyn(self, file, gather=True, compatible_output=True):
        pass

    def print_v(self, file, gather=True, compatible_output=True):
        pass

    def randomInit(self, rand_distr):
        pass

    def record(self, to_file=True):
        pass

    def record_gsyn(self, to_file=True):
        pass

    def record_v(self, to_file=True):
        pass

    def rset(self, parametername, rand_distr):
        pass

    def sample(self, n, rng=None):
        pass

    def save_positions(self, file):
        pass

    def set(self, param, val=None):
        pass

    def tset(self, parametername, value_array):
        pass
