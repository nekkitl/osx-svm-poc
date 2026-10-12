# Сборка и запуск

Документ описывает разработческий workflow. Он не выполняет установку kext за читателя и не меняет настройки безопасности системы.

## Требования

- x86_64 AMD Hackintosh с CPU, где включён AMD SVM и доступен Nested Paging (NPT). Одна лишь строка SVM в CPUID не доказывает, что firmware/macOS разрешают выполнить `VMRUN`.
- macOS на Intel-compatible x86_64. Записанная аппаратная проверка проводилась на macOS Tahoe / Darwin 25 и Ryzen 7 5700X.
- Xcode Command Line Tools, `xcrun`, macOS SDK и clang.
- Для KPI import audit — соответствующий исходник XNU/KDK с экспортами `all-kpi.exp`. Совпадение версии особенно важно для внутреннего KPI.
- Администраторский доступ к IOUserClient для запросов. Для загрузки стороннего kext macOS может требовать отдельную подпись, разрешение владельца и перезагрузку.

## 1. Собрать клиент и kext bundle

Из корня репозитория:

```sh
cd src/amdvm-provider
make
```

Ожидаемые выходы:

- `build/amdvm-probe` — user-space клиент.
- `build/AMDVMProbe.kext/` — собранный bundle.

Цель `make` только компилирует. Она не подписывает, не устанавливает и не загружает bundle, не меняет настройки загрузки и не исполняет `VMRUN`.

Если системный SDK/KPI изменился, сначала сверьте compile и import audit. Например, когда имеется подходящий `all-kpi.exp`:

```sh
make hlt-run-kpi-audit XNU_KPI_EXPORTS=/path/to/all-kpi.exp
```

Успешный локальный audit не заменяет проверку привязки bundle к активному kernel и разрешение загрузки macOS.

## 2. Source-level проверки

Makefile содержит независимые проверки C/C++ helpers, некоторые с AddressSanitizer и UndefinedBehaviorSanitizer. Примеры:

```sh
make vmcb-state-roundtrip-test
make npt-isolation-test
make npt-capacity-test
make quarantine-registry-test
make vcpu-lifecycle-test
```

Они не загружают kext и не выполняют привилегированные инструкции. Каждый target проверяет только указанную программную логику. Сборка тестов требует Xcode clang и SDK.

## 3. Загрузить bundle на тестовой системе

Установка/загрузка зависит от версии macOS, подписи и настроек конкретного компьютера. Сначала изучите политику Apple для вашей конфигурации, подпишите именно свой собранный bundle и проверьте его зависимости, идентичность и KPI imports. macOS может потребовать явного разрешения стороннего системного ПО и reboot.

Этот PoC не предлагает отключать SIP/AMFI или менять boot policy. Не копируйте системные UUID, хэши и команды из чужих отчётов: они относятся к старым сборкам. Перед любым аппаратным экспериментом убедитесь, что загружен именно проверяемый bundle и ABI клиента совпадает с ABI kext. При несовпадении версий клиент должен остановиться.

## 4. Идти от read-only к аппаратному входу

Далее команды даны относительно корня репозитория. Клиент ищет `AMDVMProbeService`; если kext не загружен или service не зарегистрирован, операция завершится ошибкой. Запросы доступа к service администраторские.

```sh
sudo ./src/amdvm-provider/build/amdvm-probe
```

Без аргументов клиент делает read-only CPUID/MSR query по online CPU. Успешный query подтверждает capability/state snapshot, но сам по себе не включает SVM и не доказывает, что `VMRUN` работает.

Два следующих шага готовят структуру контекста без аппаратного входа:

```sh
sudo ./src/amdvm-provider/build/amdvm-probe --hlt-preflight
sudo ./src/amdvm-provider/build/amdvm-probe --hlt-fpu-preflight
```

Они сообщают об аллокациях/NPT и XSTATE/FPU preflight. Оба пути должны явно вывести `vmrun_executed=no`. Это проверка предусловий, не запуск гостя.

### Экспериментальный `VMRUN` HLT

Обычная сборка fail-closed. Отдельный opt-in target компилирует фиксированный one-shot HLT путь:

```sh
cd src/amdvm-provider
make hlt-run-kpi-audit XNU_KPI_EXPORTS=/path/to/all-kpi.exp
```

После отдельной проверки и разрешённой загрузки именно этого кандидата клиентский вызов выглядит так:

```sh
sudo ./src/amdvm-provider/build/hlt-run/amdvm-probe --hlt-run --confirm-no-kdp-watchpoints
```

Эта команда выполняет реальный `VMRUN`. Она предназначена только для узкого fixed-HLT guest и остаётся экспериментальной. Перед запуском убедитесь, что не активны kernel debugger и KDP hardware watchpoints; флаг подтверждения — утверждение оператора, клиент не обнаруживает эти условия самостоятельно. Имейте резервную копию и доступный путь восстановления системы. Не запускайте произвольный payload, ОС или QEMU через этот selector.

Исторический аппаратный результат этого smoke: `vmrun_status=0`, `exit_code=0x78` (`VMEXIT_HLT`), destroy успешен, shell восстановлен. Результат относится к конкретной сборке, загруженной в октябре 2026; он не гарантирует, что новая сборка автоматически эквивалентна.

## Что считается успехом

Для source-level target успех означает лишь, что конкретный helper прошёл конкретную проверку. Для hardware smoke нужны все условия: версия ABI совпала, свежий query показывает SVM/NPT, FPU preflight прошёл, exit code равен `0x78`, lease/context успешно освобождены и клиент вернулся в shell. Записывайте UUID/hash загруженного bundle и точные версии ОС/SDK рядом с результатом.

Не интерпретируйте эти проверки как доказательство успешного Alpine boot, SMP, защиты от всех kernel fault или пригодности для production. Полноценный guest и QEMU-интеграция не входят в этот пакет.
