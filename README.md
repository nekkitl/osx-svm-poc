# osx-svm-poc

**Исследовательский AMD SVM provider для x86_64 macOS на AMD Hackintosh.**

[English](docs/en/README.md) · **Русский**

> **Статус:** proof of concept, а не готовый гипервизор. На Ryzen 7 5700X под macOS Tahoe аппаратно подтверждён ограниченный переход `VMRUN → VMEXIT_HLT → XNU → userspace`. QEMU и полноценная гостевая ОС в этот проект не входят.

## Что опубликовано

- `src/amdvm-provider/` — IOKit kext, общий ABI, SVM/VMCB/NPT helpers и mock-заголовки.
- `src/tools/` — source-level проверки, подключённые к Makefile provider.
- `docs/ru/` и `docs/en/` — описание задачи, архитектуры, результатов, ограничений, сборки и запуска.

Kext собирается как opt-in эксперимент для x86_64. В проекте нет автоматической установки/загрузки и скриптов изменения SIP или параметров загрузки. Успешная сборка не означает, что kext можно загрузить на конкретной системе.

## Начните отсюда

- [Руководство и результаты](docs/ru/README.md)
- [Сборка и запуск](docs/ru/build-and-run.md)
- [English guide](docs/en/README.md)
- [Build and run](docs/en/build-and-run.md)
