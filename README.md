# dteegen

## quick start

It takes only 6 steps to get started!

TIPS: you can copy paste the commands to run.

1. Clone this repo.

```shell
git clone https://github.com/iku-iku-iku/dteegen.git
cd dteegen
git submodule update --init --recursive
```

2. Build OpenHarmony.

If you don't want to build OpenHarmony on your own, you can use the prebuilt images provided by us.

```shell
bash ./scripts/download_prebuilt.sh
export OH_HOME=`pwd`/polyos
export OH_IMAGES=$OH_HOME/out/riscv64_virt/packages/phone/images
```

If you want to build OpenHarmony on your own, you can follow the steps below.

```shell
# install dependencies
sudo apt install git-lfs
sudo apt install repo
sudo apt install qemu-system qemu
mkdir polyos && cd polyos
export OH_HOME=`pwd`
# download the source code
git config --global credential.helper 'cache --timeout=3600'
repo init -u https://isrc.iscas.ac.cn/gitlab/riscv/polyosmobile/ohos_qemu/manifest.git -b OpenHarmony-3.2-Release --no-repo-verify
repo sync -j$(nproc) -c && repo forall -c 'git lfs pull'
docker run -it --rm -v $(pwd):/polyos-mobile --workdir /polyos-mobile swr.cn-south-1.myhuaweicloud.com/openharmony-docker/openharmony-docker:1.0.0
bash build/prebuilts_download.sh
bash build.sh --product-name qemu_riscv64_virt_linux_system --ccache
export OH_IMAGES=$OH_HOME/out/riscv64_virt/packages/phone/images
```

3. Build opensbi and penglai driver.

```shell
cd Penglai-Enclave-sPMP
export PENGLAI_HOME=`pwd`
# build opensbi
bash ./build_opensbi.sh
# build the driver
bash ./scripts/build_driver_for_oh.sh
cd ..
```

4. Use dteegen for quick demo starter.

```shell
# download dteegen
curl -o dteegen https://raw.githubusercontent.com/iku-iku-iku/dteegen/master/scripts/all_in_one.sh
chmod +x dteegen
sudo mv dteegen /usr/local/bin
# create new project
export PROJECT_NAME=new_project
export PROJECT_PATH=`pwd`/$PROJECT_NAME
dteegen create $PROJECT_NAME

# build project locally
#**NOTE**: this step is optional. Just for demonstrating that you can use cmake to build project locally without bothering with distribution and tee.
#dteegen build $PROJECT_NAME
#./$PROJECT_NAME/build/insecure/client

# deploy project
# This step will generate a folder named new_project.generated and build the generated project.
dteegen deploy $PROJECT_NAME
# then you can see your target(e.g. client, server, enclave.signed.so) in path like new_project.generated/build
```


5. Do some preparation for running OpenHarmony.

```shell
# copy opensbi to OH_HOME
cp $PENGLAI_HOME/opensbi-1.2/build-oe/qemu-virt/platform/generic/firmware/fw_jump.bin $OH_HOME
# copy scripts to OH_HOME
cp $PENGLAI_HOME/scripts/start_server.sh $OH_HOME
cp $PENGLAI_HOME/scripts/start_client.sh $OH_HOME
# copy penglai driver to OH's /data
export MOUNT_PATH=/tmp/mount
mkdir -p $MOUNT_PATH
sudo mount -o loop $OH_IMAGES/userdata.img $MOUNT_PATH
cp $PENGLAI_HOME/penglai-enclave-driver/penglai.ko $MOUNT_PATH
sudo umount $MOUNT_PATH
# inject depencies to OpenHarmony. NOTE: This step is optional since we have installed all libs in the provisoned images.
mkdir -p $MOUNT_PATH
sudo mount -o loop $OH_IMAGES/system.img $MOUNT_PATH
dteegen inject $MOUNT_PATH $PROJECT_PATH
sudo umount $MOUNT_PATH
# inject built files to OpenHarmony images
mkdir -p $MOUNT_PATH
sudo mount -o loop $OH_IMAGES/userdata.img $MOUNT_PATH
sudo cp "$PROJECT_PATH".generated/build/server $MOUNT_PATH
sudo cp "$PROJECT_PATH".generated/build/client $MOUNT_PATH
sudo cp "$PROJECT_PATH".generated/build/enclave.signed.so $MOUNT_PATH
sudo umount $MOUNT_PATH
# Since instances can not share the same images, we need to copy them.
sudo rm -rf "$OH_IMAGES"{1,2}
sudo cp -r "$OH_IMAGES"{,1}
sudo cp -r "$OH_IMAGES"{,2}
# create bridge
sudo ip link add name br0 type bridge
sudo ip link set dev br0 up
sudo ip addr add 192.168.1.109/24 dev br0
sudo iptables -P FORWARD ACCEPT
```

6. Run server and client.

NOTE: tmux is recommended to run server and client separately. 
```shell
# --------------------------------
# run server with tee ability
cd $OH_HOME
./start_server.sh
# in OH
cd data
insmod penglai.ko
./server

# --------------------------------
# run client without tee ability

cd $OH_HOME
./start_client.sh
# in OH
cd data
./client

```
