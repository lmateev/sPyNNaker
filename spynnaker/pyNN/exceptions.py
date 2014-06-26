__author__ = 'stokesa6'
class SpynnakerException(Exception):
    """Superclass of all exceptions from the pynn module.

    :raise None: does not raise any known exceptions"""
    pass


class ConfigurationException(SpynnakerException):
    """raised when the pynn front end determines a input param is invalid

    :raise None: does not raise any known exceptions"""
    pass