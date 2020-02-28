#!/bin/bash

## ======================================================================== ##
## Copyright 2009-2020 Intel Corporation                                    ##
##                                                                          ##
## Licensed under the Apache License, Version 2.0 (the "License");          ##
## you may not use this file except in compliance with the License.         ##
## You may obtain a copy of the License at                                  ##
##                                                                          ##
##     http://www.apache.org/licenses/LICENSE-2.0                           ##
##                                                                          ##
## Unless required by applicable law or agreed to in writing, software      ##
## distributed under the License is distributed on an "AS IS" BASIS,        ##
## WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. ##
## See the License for the specific language governing permissions and      ##
## limitations under the License.                                           ##
## ======================================================================== ##

# terminate if some error occurs
set -e

# check version of symbols
function check_symbols
{
  for sym in `nm $1 | grep $2_`
  do
    if [ ${#sym} -le 1 ]; then
        continue;
    fi;
    version=(`echo $sym | sed 's/.*@@\(.*\)$/\1/g' | grep -E -o "[0-9]+"`)
    if [ ${#version[@]} -ne 0 ]; then
      if [ ${#version[@]} -eq 1 ]; then version[1]=0; fi
      if [ ${#version[@]} -eq 2 ]; then version[2]=0; fi
      #echo $sym
      #echo "version0 = " ${version[0]}
      #echo "version1 = " ${version[1]}
      #echo "version2 = " ${version[2]}
      if [ ${version[0]} -gt $3 ]; then
        echo "Error: problematic $2 symbol " $sym
        exit 1
      fi
      if [ ${version[0]} -lt $3 ]; then continue; fi

      if [ ${version[1]} -gt $4 ]; then
        echo "Error: problematic $2 symbol " $sym
        exit 1
      fi
      if [ ${version[1]} -lt $4 ]; then continue; fi

      if [ ${version[2]} -gt $5 ]; then
        echo "Error: problematic $2 symbol " $sym
        exit 1
      fi
    fi
  done
}

# read embree version
EMBREE_ZIP_MODE=$1
EMBREE_LIBRARY_NAME=$2
EMBREE_VERSION=$3
EMBREE_VERSION_MAJOR=$4
EMBREE_SIGN_FILE=$5

# create package
make -j 16 preinstall
#check_symbols lib${EMBREE_LIBRARY_NAME}.so GLIBC 2 4 0
check_symbols lib${EMBREE_LIBRARY_NAME}.so GLIBC 2 14 0    # GCC 4.8
check_symbols lib${EMBREE_LIBRARY_NAME}.so GLIBCXX 3 4 11
check_symbols lib${EMBREE_LIBRARY_NAME}.so CXXABI 1 3 0
make -j 16 package

if [ "$EMBREE_ZIP_MODE" == "OFF" ]; then

  # sign all RPM files
  if [ $# -eq 5 ]; then
    ${EMBREE_SIGN_FILE} embree${EMBREE_VERSION_MAJOR}-*-${EMBREE_VERSION}-*.rpm
  fi
    
  # create TGZ of RPMs
  embree_tgz=embree-${EMBREE_VERSION}.x86_64.rpm.tar.gz
  tar czf ${embree_tgz} embree${EMBREE_VERSION_MAJOR}-*-${EMBREE_VERSION}-*.rpm

fi
