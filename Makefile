# Сборка libadb: библиотека на текущей системе и deb-пакеты в контейнерах.
#
#   make build      — собрать libadb.so (и adb/adirect) на текущей системе
#   make packages   — собрать deb-пакеты для всех целевых дистрибутивов
#   make packages DISTROS=bookworm  — только для одного
#   make boringssl  — подготовить локальный кеш исходников BoringSSL
#   make test-packages — проверить собранные пакеты в чистых контейнерах
#   make clean / distclean
#
# Целевые дистрибутивы (кодовое имя = каталог в packages/):
#   bookworm -> debian:12      trixie -> debian:13
#   noble    -> ubuntu:24.04   resolute -> ubuntu:26.04

REPO     := $(CURDIR)
BUILD    := $(REPO)/build
PKGDIR   := $(REPO)/packages
BSSL_SRC := $(REPO)/third_party/boringssl-src
JOBS     ?= $(shell nproc)

DISTROS ?= bookworm trixie noble resolute

# Кодовое имя -> docker-образ.
IMAGE_bookworm := debian:12
IMAGE_trixie   := debian:13
IMAGE_noble    := ubuntu:24.04
IMAGE_resolute := ubuntu:26.04

.PHONY: all build packages test-packages boringssl clean distclean help $(DISTROS)

all: build

help:
	@sed -n '3,13p' $(firstword $(MAKEFILE_LIST))

# --- локальный кеш BoringSSL -------------------------------------------------
# Без него ExternalProject клонирует ~550 МБ в каждую новую директорию сборки,
# то есть на каждый дистрибутив заново и с обязательным доступом к сети.
boringssl: $(BSSL_SRC)/CMakeLists.txt

$(BSSL_SRC)/CMakeLists.txt:
	bash $(REPO)/scripts/fetch-boringssl.sh "$(BSSL_SRC)"

# --- сборка на текущей системе ----------------------------------------------
build: boringssl
	cmake -S $(REPO) -B $(BUILD) -DBORINGSSL_SOURCE_DIR=$(BSSL_SRC)
	cmake --build $(BUILD) -j$(JOBS)
	@echo
	@ls -la $(BUILD)/libadb.so.* 2>/dev/null || true

# --- deb-пакеты в контейнерах -----------------------------------------------
packages: boringssl $(DISTROS)
	@echo
	@echo "=== packages ==="
	@find $(PKGDIR) -name '*.deb' -printf '%p\t%s bytes\n' | sort

# Сборка одного дистрибутива. Внутри контейнера:
#   - исходники монтируются read-only, работаем в копии (пакетная сборка пишет
#     в дерево, портить рабочую копию нельзя);
#   - кеш BoringSSL монтируется отдельно и тоже read-only;
#   - --network host нужен только для apt-get, сам BoringSSL уже локальный;
#   - docker run -t: без псевдотерминала вывод контейнера буферизуется блоками
#     и в консоли появляется рывками (а при обрыве теряется вовсе);
#   - сам сценарий лежит в scripts/build-deb-in-container.sh и ничего не
#     подавляет, поэтому в консоли видно каждый шаг сборки.
$(DISTROS):
	@echo "=== building packages for $@ ($(IMAGE_$@)) ==="
	@mkdir -p $(PKGDIR)/$@
	@rm -f $(PKGDIR)/$@/*.deb
	docker run --rm -t --network host \
	    -v "$(REPO):/src:ro" \
	    -v "$(PKGDIR)/$@:/out" \
	    -v "$(BSSL_SRC):/boringssl-src:ro" \
	    -v "$(REPO)/scripts/build-deb-in-container.sh:/build.sh:ro" \
	    -e DEBIAN_FRONTEND=noninteractive \
	    -e JOBS=$(JOBS) \
	    -e CODENAME=$@ \
	    -w /work $(IMAGE_$@) bash /build.sh
	@echo "--> $(PKGDIR)/$@:"
	@ls -la $(PKGDIR)/$@/*.deb

# --- проверка пакетов в чистых контейнерах ----------------------------------
test-packages:
	bash $(REPO)/test/auto/run_test_020.sh

clean:
	rm -rf $(BUILD) $(REPO)/obj-*

distclean: clean
	rm -rf $(PKGDIR) $(BSSL_SRC)
