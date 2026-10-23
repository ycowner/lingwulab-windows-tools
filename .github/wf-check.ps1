# Temporary: validate the release workflow without running it.
#
# Pulls every `run: |` block out of the YAML using block-scalar indentation
# rules, dedents it exactly the way YAML would, and hands it to the PowerShell
# parser. That reproduces what the runner will actually execute, so a
# here-string whose closing `"@` lands off column 0 shows up here instead of as
# a failed run.

$ErrorActionPreference = 'Stop'

$path = Join-Path $PSScriptRoot 'workflows\release-toolbox.yml'
$lines = Get-Content $path

# Sanity-check the parts this script cannot verify by parsing.
$text = $lines -join "`n"
Write-Host "[cfg] runs-on      : $([regex]::Match($text,'runs-on:\s*(\S+)').Groups[1].Value)"
Write-Host "[cfg] tag triggers : $((([regex]::Match($text, "(?s)tags:\s*\r?\n\s*-\s*'([^']+)'")).Groups[1].Value))"

$fail = 0
$checked = 0
$i = 0
$label = '(unnamed)'

while ($i -lt $lines.Count) {
    $line = $lines[$i]

    # A new step name starts a context label for whatever run block follows.
    if ($line -match '^\s*-\s*name:\s*(.+?)\s*$') { $label = $Matches[1] }

    if ($line -match '^(\s*)run:\s*\|\s*$') {
        # YAML block-scalar indentation is detected from the FIRST NON-EMPTY
        # line of the block, not from the `run:` key itself. Using the key's
        # own indentation under-dedents every line by two spaces, which would
        # push a here-string's closing "@ off column 0 and report a failure
        # that does not exist on the runner.
        $indent = $lines[$i + 1].Length - $lines[$i + 1].TrimStart().Length

        $body   = New-Object System.Collections.Generic.List[string]
        $j      = $i + 1

        # Block scalar: every more-indented, non-blank line belongs to it.
        while ($j -lt $lines.Count) {
            $t = $lines[$j]
            if ($t.Trim() -eq '') { $body.Add(''); $j++; continue }
            $ind = $t.Length - $t.TrimStart().Length
            if ($ind -le $indent) { break }
            $body.Add($t.Substring($indent))
            $j++
        }

        $script = ($body -join "`n")

        # cmd blocks are not parsed here; only the PowerShell ones.
        $shellIdx = [Math]::Max(0, $i - 1)
        $isCmd = ($lines[$shellIdx] -match '^\s*shell:\s*cmd\s*$')
        if ($isCmd) {
            Write-Host "[skip] $label  (shell: cmd -- executed by cmd.exe)"
        } else {
            $errs = $null
            $null = [System.Management.Automation.Language.Parser]::ParseInput(
                        $script, [ref]$null, [ref]$errs)
            $checked++
            if ($errs.Count) {
                $fail++
                Write-Host "[FAIL] $label"
                $errs | ForEach-Object { Write-Host "       line $($_.Extent.StartLineNumber): $($_.Message)" }
            } else {
                Write-Host "[ok]   $label"
            }
        }
        $i = $j
        continue
    }
    $i++
}

Write-Host "`nParsed $checked PowerShell block(s); syntax failures: $fail"
if ($fail) { exit 1 }
