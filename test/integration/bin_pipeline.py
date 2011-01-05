#!/usr/bin/python

from random import shuffle, randint
from time import sleep
import os, socket, sys
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
from test_common import *

NUM_INTS = 5

def get_pipelined(socket):
    if (NUM_INTS < 5):
        raise ValueError("Sorry the test needs to be run with NUM_INTS >= 5")
    #we did mention this was jank didn't we?
    #sends 5 getkqs (0,1,2,3,4) and a noop
    socket.send("\x80\x0d\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x34\x80\x0d\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x31\x80\x0d\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x33\x80\x0d\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x32\x80\x0d\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x30\x80\x0a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00")

    expected_response = "\x81\r\x00\x01\x04\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0044\x81\r\x00\x01\x04\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0011\x81\r\x00\x01\x04\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0033\x81\r\x00\x01\x04\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0022\x81\r\x00\x01\x04\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0000\x81\n\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    actual_response = socket.recv(len(expected_response))

    if actual_response != expected_response:
        raise ValueError("Pipelined get failed, expected %s, got %s" % (repr(expected_response), repr(actual_response)))

def test_function(opts, port, test_dir):
    print "Inserting"
    mc = connect_to_port(opts, port)
    insert_dict = dict((str(i),str(i)) for i in xrange(NUM_INTS))
    if (0 == mc.set_multi(insert_dict)):
        raise ValueError("Multi insert failed")
    mc.disconnect_all()

    print "Verifying (multi)"
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("localhost", port))
    get_pipelined(s)
    s.close()
    
    print "Done"

if __name__ == "__main__":
    auto_server_test_main(test_function, make_option_parser().parse(sys.argv), timeout = 2)
