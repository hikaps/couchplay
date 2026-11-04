# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 CouchPlay Contributors

FROM fedora:41

COPY scripts/install-build-deps.sh /tmp/install-build-deps.sh
RUN chmod 0755 /tmp/install-build-deps.sh \
    && /tmp/install-build-deps.sh dbus-daemon dbus-broker systemd polkit shadow-utils util-linux acl libcap \
    && dnf clean all \
    && rm -f /tmp/install-build-deps.sh

ENV container=docker
STOPSIGNAL SIGRTMIN+3
CMD ["/usr/sbin/init"]
