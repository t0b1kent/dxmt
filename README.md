# MacRunner graphics · DXMT fork

**Current component: [HyperBridge DX9 1.1.2](https://github.com/t0b1kent/dxmt/releases/tag/v1.1.2).** [MacRunner application](https://github.com/t0b1kent/macrunner-app/releases/tag/v1.1.2) · [Current game status](https://github.com/t0b1kent/macrunner-app/blob/main/docs/RELEASE-1.1.2-EN.md) · [Graphics source](https://github.com/t0b1kent/dxmt/tree/v1.1.2).

MacRunner 1.1.2 fixes an ARM64EC graphics startup regression, adds the complete DX9 display-mode catalogue, persists an explicit Retina-off choice and stops automatically enabling the experimental hidden-cursor relative mouse path. Adapter enumeration, DirectX 12 device creation and DX9 mode-query checks passed on the signed full package without launching games. The owner will download and test games after publication. Gothic 3 first-start behavior, Retina-on menu input and full Elden Ring startup still require that gameplay confirmation. The previously documented rare shutdown failure remains under investigation.

This is an unofficial MacRunner fork; the D3D12 and DX9 work is separate from and not endorsed by upstream DXMT. Original licenses and notices are retained.

## Upstream project description

A Metal-based translation layer for Direct3D 11 and 10 which allows running 3D applications on macOS using Wine.

For the current status of the project, please refer to the [project wiki](https://github.com/3Shain/dxmt/wiki).

The most recent development builds can be found [here](https://github.com/3Shain/dxmt/actions).


## Build

See [DEVELOPMENT.md](docs/DEVELOPMENT.md)
