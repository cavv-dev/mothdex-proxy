# Mothdex proxy
This is the file-serving service used by [Mothdex](https://mothdex.net). It is hosted at `proxy.mothdex.net` and serves files from torrents listed in the Mothdex database.

## Build

Install the required packages:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config wget unzip libboost-all-dev libssl-dev zlib1g-dev libzstd-dev
```

Build and install libtorrent:

```bash
wget https://github.com/arvidn/libtorrent/releases/download/v1.2.20/libtorrent-rasterbar-1.2.20.tar.gz
tar -xzf libtorrent-rasterbar-1.2.20.tar.gz
cd libtorrent-rasterbar-1.2.20
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_CXX_FLAGS="-DTORRENT_DISABLE_SUPERSEEDING -DTORRENT_DISABLE_MUTABLE_TORRENTS" ..
make -j"$(nproc)"
sudo make install
sudo ldconfig
cd ../..
```

Build and install JsonCpp:

```bash
wget https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/1.9.8.zip
unzip 1.9.8.zip
cd jsoncpp-1.9.8
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DJSONCPP_WITH_TESTS=OFF -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF -DJSONCPP_WITH_EXAMPLE=OFF
make -j"$(nproc)"
sudo make install
sudo ldconfig
cd ../..
```

Build the proxy:

```bash
make
```

## Required files

Before running it, the project directory must contain:

- `config.json`, created from `config.json.example` and filled in for the server.
- `torrent_map.json`, generated when building the Mothdex database in the [`mothdex-db`](https://github.com/cavv-dev/mothdex-db) repository.
- The `.torrent` files in `torrents/`.
- The `downloads/` directory, with enough storage for cached files.

The paths in `config.json` must match the locations of these files and directories.
