# This is a comment
FROM littlemole/devenv_clangpp_make
MAINTAINER me <little.mole@oha7.org>

# std dependencies
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y librdkafka-dev

ARG CXX=g++
ENV CXX=${CXX}

ARG BACKEND=libevent
ENV BACKEND=${BACKEND}

ARG BUILDCHAIN=make
ENV BUILDCHAIN=${BUILDCHAIN}

RUN /usr/local/bin/install.sh repro 
RUN /usr/local/bin/install.sh prio 

RUN mkdir -p /usr/local/src/repro-kafka
ADD . /usr/local/src/repro-kafka


CMD SKIPTESTS=false KAFKA=kafka /usr/local/bin/build.sh repro-kafka
