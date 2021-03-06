#!/opt/local/bin/python2.6

# This is just a bunch of tools for solving for fitting transforms

from numpy import *
from InterestIO import write_match_file, ip
import os, subprocess

def solve_euclidean(meas1, meas2):
    array1 = []
    array2 = []
    for i in meas1:
        array1.append(i.location)
    for i in meas2:
        array2.append(i.location)
    mean1 = array(array1).sum(0) / 2.0
    mean2 = array(array2).sum(0) / 2.0

    H = array([array1[0]-mean1]).transpose()*(array2[0]-mean2)
    H = H + array([array1[1]-mean1]).transpose()*(array2[1]-mean2)

    U, S, Vt = linalg.svd(H)
    rotation = dot(transpose(Vt),transpose(U))
    translation = mean2-dot(rotation,mean1)
    output = identity(3,float)
    output[0:2,0:2] = rotation
    output[0:2,2] = translation
    return output

def solve_affine(meas1, meas2):
    y = zeros((len(meas1)*2,1),float)
    A = zeros((len(meas1)*2,6),float)

    for i in range(0,len(meas1)):
        for j in range(0,2):
            row = i*2+j
            A[row,0+j*3:2+j*3] = meas1[i].location
            A[row,2+j*3] = 1
            y[row] = meas2[i][j]

    x = linalg.lstsq(A,y)[0]

    solution = identity(3,float)
    solution[0,0:3] = x[0:3,0]
    solution[1,0:3] = x[3:6,0]
    return solution

def solve_homography(meas1, meas2):
    cmd_path = os.path.realpath(__file__)
    cmd_path = cmd_path[:cmd_path.rfind("/")+1]
    write_match_file("tmp.match",meas1,meas2)
    p = subprocess.Popen(cmd_path+"homography_fit tmp.match",
                         shell=True,stdout=subprocess.PIPE);
    cmd_return = p.stdout.readline().strip()
    text = cmd_return[cmd_return.find(":")+13:].strip("((").strip("))").replace(")(",",").split(",")
    solution = identity(3,float)
    solution[0,0:3] = [float(text[0]), float(text[1]), float(text[2])]
    solution[1,0:3] = [float(text[3]), float(text[4]), float(text[5])]
    solution[2,0:3] = [float(text[6]), float(text[7]), float(text[8])]
    return solution

