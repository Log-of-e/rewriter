FROM git.zephyr-software.com:4567/opensrc/zipr/zipr-bin:latest
RUN sudo apt-get update && sudo apt install scons -y
COPY irdb-sdk /home/zuser/irdb-sdk
COPY irdb-cookbook-examples /home/zuser/irdb-cookbook-examples
RUN sudo chown zuser:zuser -R irdb*
ENV IRDB_LIBS=/opt/ps_zipr/irdb-libs/lib
ENV IRDB_SDK=/home/zuser/irdb-sdk
ENV COOKBOOK_HOME=/home/zuser/irdb-cookbook-examples
ENV PSPATH=/opt/ps_zipr/irdb-libs/plugins_install:/home/zuser/irdb-cookbook-examples/plugins_install
