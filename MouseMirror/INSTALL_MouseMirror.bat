@echo off
:: Goto current directory
cd /d "%~dp0"

:: Use DevCon for installation, since it allows providing HWID
devcon /r install MouseMirror.inf "HID\VID_045E&PID_082A&MI_00&Col01"

:: Use PnpUtil for installation (succeeds but driver isn't loaded)
::devgen /add /bus ROOT /hardwareid "HID\VID_045E&PID_082A&MI_00&Col01"
::PNPUTIL /add-driver MouseMirror.inf /install /reboot

pause
