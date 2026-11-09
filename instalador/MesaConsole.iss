; Instalador da Mesa Console (Inno Setup).
; Chamado pelo workflow ou pelo gerar-instalador.ps1, que passam a versao
; lida do Version.h. Nao rode direto.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef MySourceExe
  #define MySourceExe "..\build\MesaConsole_artefacts\Release\Mesa Console.exe"
#endif

#define MyAppName "Mesa Console"

[Setup]
; O AppId NAO MUDA ENTRE VERSOES. E ele que faz a instalacao nova
; SUBSTITUIR a anterior em vez de criar uma segunda entrada na lista de
; programas, com o desinstalador antigo apontando para arquivos que sumiram.
AppId={{8F3C21A4-6B5E-4D7A-9C12-5A3B8E1D7F20}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=Radio Metropolitana FM
VersionInfoVersion={#MyAppVersion}

DefaultDirName={autopf}\Mesa Console
DefaultGroupName=Mesa Console
DisableProgramGroupPage=yes
OutputDir=.\saida
OutputBaseFilename=MesaConsole-{#MyAppVersion}-instalador
Compression=lzma2/max
SolidCompression=yes

; 64 bits so: a mesa usa ASIO e NDI x64.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
SetupLogging=yes

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "Criar atalho na area de trabalho"; GroupDescription: "Atalhos:"
Name: "startup"; Description: "Abrir a mesa junto com o Windows"; GroupDescription: "Inicializacao:"; Flags: unchecked

[Files]
Source: "{#MySourceExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\decodifica_anuncios.py"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\Mesa Console"; Filename: "{app}\Mesa Console.exe"
Name: "{group}\Desinstalar a Mesa"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Mesa Console"; Filename: "{app}\Mesa Console.exe"; Tasks: desktopicon
Name: "{userstartup}\Mesa Console"; Filename: "{app}\Mesa Console.exe"; Tasks: startup

[Run]
Filename: "{app}\Mesa Console.exe"; Description: "Abrir a Mesa Console agora"; Flags: nowait postinstall skipifsilent

; NAO existe secao [UninstallDelete] de proposito: a configuracao em
; %APPDATA%\MesaConsole (fontes, snapshots, roteamento, ajuste de canal)
; nao e tocada nem ao instalar nem ao desinstalar. Apagar e decisao de
; quem opera, nao do desinstalador.

[Code]
// O Windows tranca o executavel em uso: atualizar com a mesa aberta
// falharia no meio, com metade dos arquivos trocados - e numa maquina de
// ar a mesa costuma estar rodando na hora da atualizacao.
function MesaEstaAberta(): Boolean;
var
  Saida: Integer;
begin
  Result := False;
  if Exec('cmd.exe',
          '/C tasklist /FI "IMAGENAME eq Mesa Console.exe" | find /I "Mesa Console.exe"',
          '', SW_HIDE, ewWaitUntilTerminated, Saida) then
    Result := (Saida = 0);
end;

function FechaMesa(): Boolean;
var
  Saida: Integer;
begin
  Exec('taskkill.exe', '/IM "Mesa Console.exe" /F', '', SW_HIDE, ewWaitUntilTerminated, Saida);
  Sleep(1500);
  Result := not MesaEstaAberta();
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if MesaEstaAberta() then
  begin
    if MsgBox('A Mesa Console esta aberta.' + #13#10#13#10 +
              'Ela precisa ser fechada para a atualizacao. Se houver programa ' +
              'no ar agora, cancele e faca isso noutro momento.' + #13#10#13#10 +
              'Fechar a mesa e continuar?', mbConfirmation, MB_YESNO) = IDYES then
    begin
      if not FechaMesa() then
        Result := 'Nao consegui fechar a Mesa Console. Feche-a na mao e tente de novo.';
    end
    else
      Result := 'Instalacao cancelada: a mesa continua aberta.';
  end;
end;

// A mesa carrega a DLL do NDI em tempo de execucao: sem o Runtime ela abre
// e funciona, so nao enxerga nem transmite NDI. Avisar aqui evita a
// descoberta no pior momento.
function TemNdiRuntime(): Boolean;
begin
  Result := FileExists(ExpandConstant('{commonpf}\NDI\NDI 6 Runtime\v6\Processing.NDI.Lib.x64.dll'))
         or FileExists(ExpandConstant('{commonpf}\NDI\NDI 5 Runtime\v5\Processing.NDI.Lib.x64.dll'))
         or FileExists(ExpandConstant('{commonpf}\NDI\NDI 6 SDK\Bin\x64\Processing.NDI.Lib.x64.dll'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and (not TemNdiRuntime()) then
    MsgBox('A Mesa Console foi instalada.' + #13#10#13#10 +
           'Observacao: o NDI Runtime nao foi encontrado nesta maquina. ' +
           'A mesa abre e funciona normalmente, mas nao vai enxergar nem ' +
           'transmitir NDI ate ele ser instalado.' + #13#10#13#10 +
           'Baixe em: ndi.video/tools', mbInformation, MB_OK);
end;
