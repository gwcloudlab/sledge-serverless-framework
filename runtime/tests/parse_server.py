import re
import os
import sys
from collections import defaultdict

#get all file names which contain key_str
def file_name(file_dir, key_str): 
    throughput_table = defaultdict(list)
    throughput_generator = defaultdict(list)
    fib_rss = defaultdict(list)
    empty_rss = defaultdict(list)
    rss_table = defaultdict(list)
    errors_table = defaultdict(list)
    for root, dirs, files in os.walk(file_dir):
        if root != os.getcwd():
            continue
        for file_i in files:
            if file_i.find(key_str) >= 0:
                suffix = file_i.split('-')      
                segs = suffix[1].split('.')
                cores_num = segs[0]
                get_values(cores_num, file_i, throughput_table,throughput_generator, errors_table)
                get_rss(rss_table, fib_rss, empty_rss,file_i, cores_num)
                #file_table[key].append(file_i)
        s_result = sorted(throughput_table.items())
        rss_result = sorted(rss_table.items())
        empty_result = sorted(empty_rss.items())
        fib_result = sorted(fib_rss.items())
        generator_result = sorted(throughput_generator.items())

        #for i in range(len(s_result)):
        #    print(s_result[i], "errors request:", errors_table[s_result[i][0]])
        print("Worker Throughput")
        for i in range(len(s_result)):
            print(int(float(((s_result[i][1][0])))),end=" ")
        print()
        print()
        print("Global RTT")
        for i in range(len(rss_result)):
            print(int(int(((rss_result[i][1][0])))),end=" ")
        print()
        print()
        print("Empty RTT")
        for i in range(len(empty_result)):
            print(int(int(((empty_result[i][1][0])))),end=" ")
        print()
        print()
        print("Fib RTT")
        for i in range(len(fib_result)):
            print(int(int(((fib_result[i][1][0])))),end=" ")
        print()
        print()
        print("Generator Througjput")
        for i in range(len(generator_result)):
            print(int(int(((generator_result[i][1][0])))),end=" ")
        print()

def get_rss(rss_table,fib_rss, empty_rss, file_name, core):
    fo = open(file_name, "r+")
    total_rss = 0
    total_sandbox = 0

    fib = 0
    empty = 0

    nb_fib = 0
    nb_empty = 0

    fib_average = 0
    empty_average = 0

    for line in fo:
        line = line.strip()
        if "throughput" in line:
            total_rss += int(line.split(" ")[3])
            total_sandbox += int(line.split(" ")[5])
            nb_fib += int(line.split(" ")[11])
            nb_empty += int(line.split(" ")[8])
            fib += int(line.split(" ")[10])
            empty += int(line.split(" ")[7])

    if total_sandbox == 0:
       rss_average = 0
    else:
        rss_average = total_rss/total_sandbox
        fib_average = fib/nb_fib
        empty_average = empty/nb_empty
  #  rss_average_per_worker = rss_average/int(core)
    empty_rss[int(core)].append(empty_average)
    fib_rss[int(core)].append(fib_average)
    rss_table[int(core)].append(rss_average)

def get_values(core, file_name, throughput_table, throughput_generator, errors_table):
    print("parse file:", file_name)
    fo = open(file_name, "r+")
    total_throughput = 0
    total_throughput_gen = 0
    for line in fo:
        line = line.strip()
        if "throughput" in line:
            i_th = float(line.split(" ")[1])
            total_throughput += i_th
        elif "creation" in line:
            i = float(line.split(" ")[2])
            total_throughput_gen += i
    throughput_generator[int(core)].append(total_throughput_gen)
    throughput_table[int(core)].append(total_throughput)
    #cmd2='grep "throughput is" %s | awk \'{print $7}\'' % file_name
    #rt2=os.popen(cmd2).read().strip()
    #if len(rt2) != 0:
    #    errors = rt2.splitlines()[0]
    #    errors_table[int(core)].append(int(errors))
    #else:
    #    errors_table[int(core)].append(0)
    #print(file_name, rt2)
    

if __name__ == "__main__":
    import json
    argv = sys.argv[1:]
    if len(argv) < 1:
        print("usage ", sys.argv[0], " file containing key word")
        sys.exit()

    file_name(os.getcwd(), argv[0])


