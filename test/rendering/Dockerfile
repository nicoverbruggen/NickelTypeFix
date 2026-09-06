# Pin Qt as well as the compiler: the regression checks this QtGui build's shaper selector.
FROM ghcr.io/nicoverbruggen/nickelbench@sha256:d04d7fdcdd5422809b487972beb2d9b6ff6ed7e9552b1b6091851189aca2049e
COPY dependencies.tsv /tmp/dependencies.tsv
RUN set -eu; \
    mkdir -p /opt/rendering/fonts /opt/rendering/runtime /opt/rendering/qemu /tmp/downloads; \
    cd /tmp/downloads; \
    while read -r checksum name url; do \
        case "$checksum" in ''|'#'*) continue ;; esac; \
        curl --fail --silent --show-error --location --retry 3 "$url" --output "$name"; \
        printf '%s  %s\n' "$checksum" "$name" | sha256sum --check -; \
    done < /tmp/dependencies.tsv; \
    dpkg-deb --extract qemu.deb /opt/rendering/qemu; \
    find /opt/rendering/qemu/usr/bin -type f ! -name qemu-arm-static -delete; \
    dpkg-deb --extract libc.deb /opt/rendering/runtime; \
    dpkg-deb --extract dejavu.deb /tmp/dejavu; \
    mv /tmp/dejavu/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /opt/rendering/fonts/; \
    mv /tmp/dejavu/usr/share/doc/fonts-dejavu-core/copyright /opt/rendering/fonts/DejaVu-LICENSE.txt; \
    rm -rf /tmp/dejavu; \
    mv ./*.ttf ./*-OFL.txt /opt/rendering/fonts/; \
    rm -rf /tmp/downloads /tmp/dependencies.tsv
WORKDIR /tmp
