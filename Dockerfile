FROM --platform=linux/amd64 python:3.12.16-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df AS build

WORKDIR /app
RUN apt-get update \
    && test -s /var/lib/apt/lists/deb.debian.org_debian_dists_bookworm_main_binary-amd64_Packages.lz4 \
    && apt-get install -y --no-install-recommends g++ \
    && rm -rf /var/lib/apt/lists/*

COPY native_scan.cpp /app/
RUN c++ -O3 -std=c++17 -fPIC -Wall -Wextra -shared /app/native_scan.cpp -o /app/libmvdscan.so

FROM --platform=linux/amd64 python:3.12.16-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df

WORKDIR /app
ENV PYTHONUNBUFFERED=1
RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /data
COPY minivectordb.py /app/
COPY --from=build /app/libmvdscan.so /app/libmvdscan.so

VOLUME ["/data"]
ENTRYPOINT ["python3", "/app/minivectordb.py"]
CMD ["--data-dir", "/data"]
