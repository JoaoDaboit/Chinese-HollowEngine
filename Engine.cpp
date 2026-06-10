#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <cstdio>
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"
#include <algorithm>
#include <array>
#include <vector>
#include <string>

// Função para abrir diálogo de seleção de arquivo (Linux/Unix) - apenas PNG e JPG
std::string abrirSeletorArquivo(const char* titulo = "Selecionar Arquivo") {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "zenity --file-selection --title=\"%s\" --file-filter='Images|*.png *.jpg *.jpeg' 2>/dev/null", titulo);
    
    FILE* fp = popen(cmd, "r");
    if (!fp) return "";
    
    char buffer[1024] = {0};
    if (fgets(buffer, sizeof(buffer) - 1, fp)) {
        pclose(fp);
        // Remove a quebra de linha no final
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        return std::string(buffer);
    }
    pclose(fp);
    return "";
}

// Carregador de textura com fallback seguro
SDL_Texture* carregarTexturaDoDisco(SDL_Renderer* renderer, const char* caminho) {
    if (!caminho || strlen(caminho) == 0) {
        return NULL;
    }
    
    SDL_Surface* surface = IMG_Load(caminho);
    if (!surface) {
        fprintf(stderr, "Erro ao carregar textura: %s (IMG_Load falhou: %s)\n", caminho, IMG_GetError());
        return NULL;
    }
    
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    
    if (!texture) {
        fprintf(stderr, "Erro ao criar textura: %s (SDL_CreateTextureFromSurface falhou: %s)\n", caminho, SDL_GetError());
        return NULL;
    }
    
    return texture;
}

// Libera textura com segurança
void liberarTextura(SDL_Texture*& tex) {
    if (tex) {
        SDL_DestroyTexture(tex);
        tex = NULL;
    }
}

int jogo_rodando = 1;

// Estruturas para matemática 3D
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    Vec3 operator+(const Vec3& v) const { return Vec3(x + v.x, y + v.y, z + v.z); }
    Vec3 operator-(const Vec3& v) const { return Vec3(x - v.x, y - v.y, z - v.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    float dot(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    Vec3 cross(const Vec3& v) const { return Vec3(y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x); }
    float length() const { return sqrtf(x * x + y * y + z * z); }
    Vec3 normalize() const { float len = length(); if (len > 0) return Vec3(x / len, y / len, z / len); return *this; }
};

struct Vec2 {
    float x, y;
    Vec2() : x(0), y(0) {}
    Vec2(float x, float y) : x(x), y(y) {}
};

// Câmera 3D
struct Camera {
    Vec3 pos, front, up;
    float yaw, pitch;
    
    Camera() : pos(0, 3, 5), front(0, -0.5, -1), up(0, 1, 0), yaw(-90), pitch(-30) {}
    
    void atualizarDirecao() {
        front.x = cosf(yaw * 3.14159f / 180.0f) * cosf(pitch * 3.14159f / 180.0f);
        front.y = sinf(pitch * 3.14159f / 180.0f);
        front.z = sinf(yaw * 3.14159f / 180.0f) * cosf(pitch * 3.14159f / 180.0f);
        front = front.normalize();
    }
};

// Luz direcional + posicional (bloco de iluminação)
struct LuzDirecional
{
    Vec3 direcao;
    Vec3 posicao;      // posição do bloco de luz no mundo
    float intensidade;
    int tipo;          // mantém estrutura para código legado, mas a luz é sempre posicional
    
    LuzDirecional() : direcao(0.0f, -1.0f, 0.0f), posicao(2.0f, 5.0f, 2.0f), intensidade(1.0f), tipo(1) {}
};

// per-object lights removed (handled by scene light only)

// Material PBR simples (nomes de textura + ponteiros para futuras texturas SDL)
struct MaterialPBR
{
    std::string albedo;
    std::string normal;
    std::string roughness;

    // futuros ponteiros de textura (NULL por enquanto)
    SDL_Texture* albedoTex;
    SDL_Texture* normalTex;
    SDL_Texture* roughnessTex;
    // Observação importante:
    // - `normalTex` e `roughnessTex` existem para compatibilidade com um fluxo PBR,
    //   mas o renderizador atual usa `SDL_RenderGeometry` (rasterização 2D via CPU)
    //   e não possui suporte a shaders por fragmento. Portanto NÃO se deve
    //   esperar que `normalTex` ou `roughnessTex` influenciem o cálculo de luz
    //   atual (que é feito por vértice na função `calcularIluminacaoFace`).
    //   Essas texturas são armazenadas para futuro uso em pipelines que suportem
    //   shaders (GPU) — por enquanto, elas são ignoradas nos cálculos de iluminação.

    MaterialPBR() : albedo(""), normal(""), roughness(""), albedoTex(NULL), normalTex(NULL), roughnessTex(NULL) {}
};

// Tipos de objeto 3D suportados
enum BlocoTipo {
    BLOCO_CUBO = 0,
    BLOCO_PLANO = 1,
    BLOCO_CONE = 2,
    BLOCO_ESFERA = 3,
};

// Objeto 3D — agora com tipo e material
struct Cube {
    Vec3 pos;
    Vec3 escala;
    Vec3 rot;
    int selecionado;
    uint32_t cor;
    int tipo;
    MaterialPBR material;

    Cube(Vec3 p = Vec3(0, 0, 0), float t = 1.0f, int tipoObj = BLOCO_CUBO)
        : pos(p), escala(t, t, t), rot(0, 0, 0), selecionado(0), cor(0xFF0080FF), tipo(tipoObj), material() {}
};

// Enum para modo de transformação do gizmo
enum GizmoMode {
    GIZMO_NONE = 0,
    GIZMO_MOVE_X = 1,
    GIZMO_MOVE_Y = 2,
    GIZMO_MOVE_Z = 3,
    GIZMO_SCALE_X = 4,
    GIZMO_SCALE_Y = 5,
    GIZMO_SCALE_Z = 6,
    GIZMO_ROT_X = 7,
    GIZMO_ROT_Y = 8,
    GIZMO_ROT_Z = 9,
};

struct GizmoState {
    int modo;
    int cuboSelecionado;
    ImVec2 mouseAnterior;
    bool arrastando;
};

static bool gizmoEhMovimento(int modo) {
    return modo >= GIZMO_MOVE_X && modo <= GIZMO_MOVE_Z;
}

static bool gizmoEhEscala(int modo) {
    return modo >= GIZMO_SCALE_X && modo <= GIZMO_SCALE_Z;
}

static bool gizmoEhRotacao(int modo) {
    return modo >= GIZMO_ROT_X && modo <= GIZMO_ROT_Z;
}

static int proximoTipoGizmo(int modo) {
    if (modo == GIZMO_NONE || gizmoEhRotacao(modo)) return GIZMO_MOVE_X;
    if (gizmoEhMovimento(modo)) return GIZMO_SCALE_X;
    if (gizmoEhEscala(modo)) return GIZMO_ROT_X;
    return GIZMO_MOVE_X;
}

// Cores dos eixos (RGB)
const uint32_t COR_X = 0xFF0000;  // Vermelho
const uint32_t COR_Y = 0x00FF00;  // Verde
const uint32_t COR_Z = 0x00FFFF;  // Azul

Vec2 projetarPonto3D(Vec3 ponto, Camera& cam, int w, int h) {
    Vec3 relativo = ponto - cam.pos;
    
    // Criar base de coordenadas da câmera
    Vec3 right = cam.front.cross(cam.up).normalize();
    Vec3 novoUp = right.cross(cam.front).normalize();
    
    // Converter para coordenadas da câmera
    float dist = relativo.dot(cam.front);
    float x = relativo.dot(right);
    float y = relativo.dot(novoUp);
    
    if (dist <= 0.1f) dist = 0.1f;
    
    float fov = 45.0f * 3.14159f / 180.0f;
    float scale = (float)h / (2.0f * tanf(fov / 2.0f) * dist);
    
    Vec2 resultado;
    resultado.x = (float)w / 2.0f + x * scale;
    resultado.y = (float)h / 2.0f - y * scale;
    
    return resultado;
}

float worldUnitsForPixels(Camera& cam, const Vec3& pos, int h, float desiredPixels) {
    float fov = 45.0f * 3.14159f / 180.0f;
    float dist = (pos - cam.pos).length();
    if (dist <= 0.1f) dist = 0.1f;
    float pixelPerUnit = (float)h / (2.0f * tanf(fov / 2.0f) * dist);
    if (pixelPerUnit <= 0.0001f) pixelPerUnit = 0.0001f;
    return desiredPixels / pixelPerUnit;
}

void desenharLinha3D(SDL_Renderer* pintor, Vec3 p1, Vec3 p2, Camera& cam, int w, int h, uint32_t cor) {
    Vec2 sp1 = projetarPonto3D(p1, cam, w, h);
    Vec2 sp2 = projetarPonto3D(p2, cam, w, h);
    SDL_SetRenderDrawColor(pintor, (cor >> 16) & 0xFF, (cor >> 8) & 0xFF, cor & 0xFF, 255);
    SDL_RenderDrawLineF(pintor, sp1.x, sp1.y, sp2.x, sp2.y);
}

void desenharLinha3DGrossa(SDL_Renderer* pintor, Vec3 p1, Vec3 p2, Camera& cam, int w, int h, uint32_t cor, float espessura) {
    Vec2 sp1 = projetarPonto3D(p1, cam, w, h);
    Vec2 sp2 = projetarPonto3D(p2, cam, w, h);
    Vec2 dir = {sp2.x - sp1.x, sp2.y - sp1.y};
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    Vec2 perp = {0.0f, 0.0f};
    if (len > 0.001f) {
        perp.x = -dir.y / len;
        perp.y = dir.x / len;
    }
    SDL_SetRenderDrawColor(pintor, (cor >> 16) & 0xFF, (cor >> 8) & 0xFF, cor & 0xFF, 255);
    int steps = (int)fmaxf(1.0f, espessura);
    for (int i = -steps; i <= steps; i++) {
        float offset = perp.x * i * 0.6f;
        float offsetY = perp.y * i * 0.6f;
        SDL_RenderDrawLineF(pintor, sp1.x + offset, sp1.y + offsetY, sp2.x + offset, sp2.y + offsetY);
    }
}

bool detectarLuzHandle(ImVec2 mousePos, Camera& cam, const LuzDirecional& luz, int w, int h) {
    Vec2 proj = projetarPonto3D(luz.posicao, cam, w, h);
    float dx = proj.x - mousePos.x;
    float dy = proj.y - mousePos.y;
    float dist = sqrtf(dx * dx + dy * dy);
    return dist < 18.0f;
}

void desenharLuzHandle(SDL_Renderer* pintor, Camera& cam, const LuzDirecional& luz, int w, int h, bool selected) {
    Vec2 proj = projetarPonto3D(luz.posicao, cam, w, h);
    int tamanho = selected ? 18 : 14;
    int half = tamanho / 2;
    SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_BLEND);
    SDL_Color cor = selected ? SDL_Color{255, 220, 0, 255} : SDL_Color{255, 180, 0, 220};
    SDL_SetRenderDrawColor(pintor, cor.r, cor.g, cor.b, cor.a);
    SDL_Rect rect = {(int)(proj.x - half), (int)(proj.y - half), tamanho, tamanho};
    SDL_RenderFillRect(pintor, &rect);
    SDL_SetRenderDrawColor(pintor, 255, 255, 255, 255);
    SDL_RenderDrawRect(pintor, &rect);
}

void desenharTrianguloPreenchido(SDL_Renderer* pintor, Vec2 a, Vec2 b, Vec2 c, Vec2 ta, Vec2 tb, Vec2 tc, uint32_t cor, SDL_Texture* texture = NULL) {
    SDL_Vertex verts[3];
    SDL_Color color = {(uint8_t)((cor >> 16) & 0xFF), (uint8_t)((cor >> 8) & 0xFF), (uint8_t)(cor & 0xFF), (uint8_t)((cor >> 24) & 0xFF)};

    // Usa as coordenadas UV passadas (agora não mais hardcoded)
    verts[0].position = {a.x, a.y}; verts[0].color = color; verts[0].tex_coord = {ta.x, ta.y};
    verts[1].position = {b.x, b.y}; verts[1].color = color; verts[1].tex_coord = {tb.x, tb.y};
    verts[2].position = {c.x, c.y}; verts[2].color = color; verts[2].tex_coord = {tc.x, tc.y};
    SDL_RenderGeometry(pintor, texture, verts, 3, NULL, 0);
}

void desenharQuadPreenchido(SDL_Renderer* pintor, Vec2 a, Vec2 b, Vec2 c, Vec2 d, uint32_t cor, SDL_Texture* texture = NULL) {
    // Mapeamento UV da face quad: a->(0,0), b->(1,0), c->(1,1), d->(0,1)
    Vec2 ta(0.0f, 0.0f), tb(1.0f, 0.0f), tc(1.0f, 1.0f), td(0.0f, 1.0f);
    desenharTrianguloPreenchido(pintor, a, b, c, ta, tb, tc, cor, texture);
    desenharTrianguloPreenchido(pintor, a, c, d, ta, tc, td, cor, texture);
}

Vec3 aplicarRotacao(const Vec3& ponto, const Vec3& centro, const Vec3& rot) {
    Vec3 p = ponto - centro;
    float radX = rot.x * 3.14159f / 180.0f;
    float radY = rot.y * 3.14159f / 180.0f;
    float radZ = rot.z * 3.14159f / 180.0f;

    float y1 = p.y * cosf(radX) - p.z * sinf(radX);
    float z1 = p.y * sinf(radX) + p.z * cosf(radX);
    p.y = y1; p.z = z1;

    float x2 = p.x * cosf(radY) + p.z * sinf(radY);
    float z2 = -p.x * sinf(radY) + p.z * cosf(radY);
    p.x = x2; p.z = z2;

    float x3 = p.x * cosf(radZ) - p.y * sinf(radZ);
    float y3 = p.x * sinf(radZ) + p.y * cosf(radZ);
    p.x = x3; p.y = y3;

    return centro + p;
}

struct FaceDraw {
    Vec3 v0, v1, v2, v3;
    Vec3 centro;
    Vec3 normal;
    float depth;
};

bool faceVisivel(const Vec3& centro, const Vec3& normal, const Camera& cam) {
    Vec3 viewDir = (cam.pos - centro).normalize();
    return normal.dot(viewDir) > 0.0f;
}

void desenharFace3D(SDL_Renderer* pintor, const Vec3& v0, const Vec3& v1, const Vec3& v2, const Vec3& v3, Camera& cam, int w, int h, uint32_t corFace, SDL_Texture* texture = NULL) {
    Vec2 p0 = projetarPonto3D(v0, cam, w, h);
    Vec2 p1 = projetarPonto3D(v1, cam, w, h);
    Vec2 p2 = projetarPonto3D(v2, cam, w, h);
    Vec2 p3 = projetarPonto3D(v3, cam, w, h);
    // Mapear os cantos da face para as coordenadas UV (0,0)-(1,1)
    desenharQuadPreenchido(pintor, p0, p1, p2, p3, corFace, texture);
}

void desenharCubo3D(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, const LuzDirecional& luz, const std::vector<Cube>& cubos);

// Calcula fator de sombra simples para um ponto baseado na luz
float calcularSombra(const Vec3& pontoMundial, const LuzDirecional& luz, float raioSombra = 2.0f) {
    if (luz.tipo == 0) {
        // Luz direcional: sem sombra próxima
        return 1.0f;
    } else {
        // Luz posicional: calcula distância e atenua
        float dist = (pontoMundial - luz.posicao).length();
        float atenuacao = 1.0f - fminf(1.0f, dist / raioSombra);
        return 0.5f + 0.5f * atenuacao;  // 0.5 a 1.0
    }
}

// Aplica a intensidade de iluminação a uma cor base por vértice.
// Quando uma textura estiver presente, essa cor de vértice será
// multiplicada pela amostra da textura em `SDL_RenderGeometry`.
// Portanto, para que a textura apareça fielmente, certifique-se de
// que a cor base seja branca neutra (0xFFFFFFFF) quando usar texturas.
uint32_t aplicarIntensidadeCor(uint32_t cor, float intensidade) {
    intensidade = fminf(intensidade, 1.0f);
    uint8_t r = (uint8_t)(fminf(255.0f, ((cor >> 16) & 0xFF) * intensidade));
    uint8_t g = (uint8_t)(fminf(255.0f, ((cor >> 8) & 0xFF) * intensidade));
    uint8_t b = (uint8_t)(fminf(255.0f, (cor & 0xFF) * intensidade));
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// per-object light drawing removed

float calcularIluminacaoFace(const Vec3& centro, const Vec3& normal, const LuzDirecional& luz, const std::vector<Cube>& cubos) {
    // Simpler lighting: ambient + single directional/positional light contribution.
    float ambiente = 0.6f;
    float intensidadeTotal = ambiente;

    Vec3 normalN = normal.normalize();
    Vec3 direcaoLuz = luz.direcao.normalize();

    if (luz.tipo == 0) {
        // directional light
        Vec3 luzParaSurface = direcaoLuz * -1.0f;
        float lambert = fmaxf(0.0f, normalN.dot(luzParaSurface));
        intensidadeTotal += lambert * luz.intensidade;
    } else {
        // positional light with basic attenuation
        Vec3 paraLuz = (luz.posicao - centro);
        float dist = paraLuz.length();
        if (dist <= 0.0001f) dist = 0.0001f;
        Vec3 dir = paraLuz.normalize();
        float lambert = fmaxf(0.0f, normalN.dot(dir));
        float atenuacao = 1.0f / (1.0f + dist * 0.1f + dist * dist * 0.05f);
        intensidadeTotal += lambert * luz.intensidade * atenuacao;
    }

    // Do not include per-object light probes (BLOCO_LUZ) to keep lighting simple and stable
    if (intensidadeTotal < 0.0f) intensidadeTotal = 0.0f;
    if (intensidadeTotal > 1.0f) intensidadeTotal = 1.0f;
    return intensidadeTotal;
}

// Mapeia o nome do material para uma cor de visualização
uint32_t obterCorPorMaterial(const MaterialPBR& material) {
    // Se uma textura foi carregada, use branco neutro como cor base.
    // Explicação: `SDL_RenderGeometry` multiplica a cor do vértice pela textura.
    // Para permitir que a textura apareça com suas cores originais e apenas
    // seja atenuada pela iluminação por vértice, retornamos branco puro.
    // NOTA: nomes como "tijolo/madeira" não são necessários aqui — a textura
    // real irá prover o aspecto visual. Mantemos 0xFFFFFFFF para não causar
    // estouro/branco excessivo quando a textura estiver presente.
    if (material.albedoTex != NULL) {
        return 0xFFFFFFFFu;
    }
    
    std::string albedo = material.albedo;
    
    // Remove extensão se existir
    size_t pos = albedo.find('.');
    if (pos != std::string::npos) {
        albedo = albedo.substr(0, pos);
    }
    
    // Mapeia nomes conhecidos para cores
    if (albedo.find("tijolo") != std::string::npos) {
        return 0xFFB84C42;  // Vermelho-tijolo
    } else if (albedo.find("madeira") != std::string::npos) {
        return 0xFF8B5A3C;  // Marrom-madeira
    } else if (albedo.find("pedra") != std::string::npos) {
        return 0xFF808080;  // Cinza-pedra
    } else if (albedo.find("grama") != std::string::npos) {
        return 0xFF2D7D2D;  // Verde-grama
    } else if (albedo.find("neon") != std::string::npos) {
        return 0xFFFF00FF;  // Magenta-neon
    } else if (albedo.find("metal") != std::string::npos) {
        return 0xFFC0C0C0;  // Prata-metal
    }
    
    return 0xFFDDDDDD;  // Branco padrão
}

void desenharTriangulo3D(SDL_Renderer* pintor, const Vec3& a, const Vec3& b, const Vec3& c, Camera& cam, int w, int h, uint32_t corFace, SDL_Texture* texture = NULL) {
    Vec2 p0 = projetarPonto3D(a, cam, w, h);
    Vec2 p1 = projetarPonto3D(b, cam, w, h);
    Vec2 p2 = projetarPonto3D(c, cam, w, h);

    if (texture == NULL) {
        // Sem textura, usa cor apenas
        Vec2 ta(0,0), tb(0,0), tc(0,0);
        desenharTrianguloPreenchido(pintor, p0, p1, p2, ta, tb, tc, corFace, texture);
        return;
    }

    // Quando há textura mas não há UVs específicos, gera UVs a partir
    // da projeção 2D do triângulo mapeando para a caixa delimitadora (0..1).
    float minX = fminf(fminf(p0.x, p1.x), p2.x);
    float minY = fminf(fminf(p0.y, p1.y), p2.y);
    float maxX = fmaxf(fmaxf(p0.x, p1.x), p2.x);
    float maxY = fmaxf(fmaxf(p0.y, p1.y), p2.y);
    float dx = maxX - minX; if (dx <= 0.0001f) dx = 1.0f;
    float dy = maxY - minY; if (dy <= 0.0001f) dy = 1.0f;

    Vec2 ta = {(p0.x - minX) / dx, (p0.y - minY) / dy};
    Vec2 tb = {(p1.x - minX) / dx, (p1.y - minY) / dy};
    Vec2 tc = {(p2.x - minX) / dx, (p2.y - minY) / dy};

    desenharTrianguloPreenchido(pintor, p0, p1, p2, ta, tb, tc, corFace, texture);
}

void desenharFacesOrdenadas(SDL_Renderer* pintor, const std::vector<Vec3>& vertices, const std::vector<std::array<int,4>>& faces, Camera& cam, int w, int h, uint32_t faceColor, const LuzDirecional& luz, const std::vector<Cube>& cubos, SDL_Texture* texture = NULL) {
    struct FaceEntry { Vec3 v0, v1, v2, v3; Vec3 normal; Vec3 centro; float depth; };
    std::vector<FaceEntry> drawFaces;
    drawFaces.reserve(faces.size());

    for (const auto& face : faces) {
        Vec3 v0 = vertices[face[0]];
        Vec3 v1 = vertices[face[1]];
        Vec3 v2 = vertices[face[2]];
        Vec3 v3 = vertices[face[3]];
        Vec3 normal = (v1 - v0).cross(v2 - v0).normalize();
        Vec3 centro = Vec3((v0.x + v1.x + v2.x + v3.x) / 4.0f,
                           (v0.y + v1.y + v2.y + v3.y) / 4.0f,
                           (v0.z + v1.z + v2.z + v3.z) / 4.0f);

        if (!faceVisivel(centro, normal, cam)) continue;

        float depth = (centro - cam.pos).dot(cam.front);
        // CLIPPING: Descartar faces atrás ou muito perto da câmera
        if (depth < 0.1f) continue;
        drawFaces.push_back({v0, v1, v2, v3, normal, centro, depth});
    }

    // CORREÇÃO: Mudar de '<' para '>' para consertar faces invertidas
    std::sort(drawFaces.begin(), drawFaces.end(), [](const FaceEntry& a, const FaceEntry& b) {
        return a.depth > b.depth; 
    });

    for (const auto& face : drawFaces) {
        float intensidadeFace = calcularIluminacaoFace(face.centro, face.normal, luz, cubos);
        uint32_t corFace = aplicarIntensidadeCor(faceColor, intensidadeFace);
        desenharFace3D(pintor, face.v0, face.v1, face.v2, face.v3, cam, w, h, corFace, texture);
    }
}

void desenharCone3D(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, const LuzDirecional& luz, const std::vector<Cube>& cubos) {
    const int segments = 20;
    float radius = cubo.escala.x * 0.5f;
    float height = cubo.escala.y;

    Vec3 apexLocal(0, height * 0.5f, 0);
    Vec3 baseCenterLocal(0, -height * 0.5f, 0);

    std::vector<Vec3> basePoints(segments);
    for (int i = 0; i < segments; i++) {
        float ang = (float)i / segments * 3.14159f * 2.0f;
        basePoints[i] = Vec3(cosf(ang) * radius, -height * 0.5f, sinf(ang) * radius);
    }

    Vec3 apex = aplicarRotacao(cubo.pos + apexLocal, cubo.pos, cubo.rot);
    Vec3 baseCenter = aplicarRotacao(cubo.pos + baseCenterLocal, cubo.pos, cubo.rot);

    uint32_t faceColor = obterCorPorMaterial(cubo.material);
    if (faceColor == 0xFFDDDDDD && cubo.cor != 0xFF0080FF) faceColor = cubo.cor;

    struct TriangleEntry {
        Vec3 a,b,c; Vec3 normal; Vec3 centro; float depth;
    };
    std::vector<TriangleEntry> faces;
    faces.reserve(segments * 2);

    for (int i = 0; i < segments; i++) {
        int j = (i + 1) % segments;
        Vec3 b0 = aplicarRotacao(cubo.pos + basePoints[i], cubo.pos, cubo.rot);
        Vec3 b1 = aplicarRotacao(cubo.pos + basePoints[j], cubo.pos, cubo.rot);

        Vec3 normalSide = (b0 - apex).cross(b1 - apex).normalize();
        Vec3 centroSide = Vec3((apex.x + b0.x + b1.x) / 3.0f, (apex.y + b0.y + b1.y) / 3.0f, (apex.z + b0.z + b1.z) / 3.0f);
        faces.push_back({apex, b1, b0, normalSide, centroSide, (centroSide - cam.pos).dot(cam.front)});

        Vec3 normalBase = (b1 - baseCenter).cross(b0 - baseCenter).normalize();
        Vec3 centroBase = Vec3((baseCenter.x + b1.x + b0.x) / 3.0f, (baseCenter.y + b1.y + b0.y) / 3.0f, (baseCenter.z + b1.z + b0.z) / 3.0f);
        faces.push_back({baseCenter, b0, b1, normalBase, centroBase, (centroBase - cam.pos).dot(cam.front)});
    }

    // CORREÇÃO: a.depth > b.depth
    std::sort(faces.begin(), faces.end(), [](const TriangleEntry& a, const TriangleEntry& b){
        return a.depth > b.depth;
    });

    for (const auto& tri : faces) {
        if (!faceVisivel(tri.centro, tri.normal, cam)) continue;
        // CLIPPING: Descartar triângulos atrás da câmera
        if (tri.depth < 0.1f) continue;
        float intensidadeFace = calcularIluminacaoFace(tri.centro, tri.normal, luz, cubos);
        desenharTriangulo3D(pintor, tri.a, tri.b, tri.c, cam, w, h, aplicarIntensidadeCor(faceColor, intensidadeFace), cubo.material.albedoTex);
    }
}

void desenharEsfera3D(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, const LuzDirecional& luz, const std::vector<Cube>& cubos) {
    const int rings = 12;
    const int segments = 16;
    float radius = cubo.escala.x * 0.5f;

    std::vector<Vec3> vertices;
    vertices.reserve((rings + 1) * (segments + 1));

    // CORREÇÃO: Grade contínua garantindo que não há buracos nas costuras
    for (int i = 0; i <= rings; i++) {
        float phi = (float)i / rings * 3.14159f; // 0 até PI
        float y = cosf(phi) * radius;
        float r = sinf(phi) * radius;

        for (int j = 0; j <= segments; j++) {
            float theta = (float)j / segments * 3.14159f * 2.0f; // 0 até 2PI
            float x = cosf(theta) * r;
            float z = sinf(theta) * r;
            vertices.push_back(aplicarRotacao(cubo.pos + Vec3(x, y, z), cubo.pos, cubo.rot));
        }
    }

    struct TriangleEntry {
        Vec3 a, b, c; Vec3 normal; Vec3 centro; float depth;
    };
    std::vector<TriangleEntry> faces;
    faces.reserve(rings * segments * 2);

    for (int i = 0; i < rings; i++) {
        for (int j = 0; j < segments; j++) {
            int i0 = i * (segments + 1) + j;
            int i1 = i0 + 1;
            int i2 = (i + 1) * (segments + 1) + j;
            int i3 = i2 + 1;

            // Triângulo 1
            Vec3 a = vertices[i0];
            Vec3 b = vertices[i2];
            Vec3 c = vertices[i1];
            Vec3 normal1 = (b - a).cross(c - a).normalize();
            Vec3 centro1 = (a + b + c) * (1.0f / 3.0f);
            faces.push_back({a, b, c, normal1, centro1, (centro1 - cam.pos).dot(cam.front)});

            // Triângulo 2
            Vec3 d = vertices[i1];
            Vec3 e = vertices[i2];
            Vec3 f = vertices[i3];
            Vec3 normal2 = (e - d).cross(f - d).normalize();
            Vec3 centro2 = (d + e + f) * (1.0f / 3.0f);
            faces.push_back({d, e, f, normal2, centro2, (centro2 - cam.pos).dot(cam.front)});
        }
    }

    // CORREÇÃO: Z-sorting com sinal de Maior (>)
    std::sort(faces.begin(), faces.end(), [](const TriangleEntry& t1, const TriangleEntry& t2) {
        return t1.depth > t2.depth;
    });

    uint32_t faceColor = obterCorPorMaterial(cubo.material);
    if (faceColor == 0xFFDDDDDD && cubo.cor != 0xFF0080FF) faceColor = cubo.cor;

    for (const auto& tri : faces) {
        if (!faceVisivel(tri.centro, tri.normal, cam)) continue;
        // CLIPPING: Descartar triângulos atrás da câmera
        if (tri.depth < 0.1f) continue;
        float intensidade = calcularIluminacaoFace(tri.centro, tri.normal, luz, cubos);
        desenharTriangulo3D(pintor, tri.a, tri.b, tri.c, cam, w, h, aplicarIntensidadeCor(faceColor, intensidade), cubo.material.albedoTex);
    }
}

void desenharCubo3D(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, const LuzDirecional& luz, const std::vector<Cube>& cubos) {
    float tx = cubo.escala.x / 2.0f;
    float ty = cubo.escala.y / 2.0f;
    float tz = cubo.escala.z / 2.0f;

    Vec3 locais[8] = {
        Vec3(-tx, -ty, -tz), Vec3(tx, -ty, -tz),
        Vec3(tx, ty, -tz),   Vec3(-tx, ty, -tz),
        Vec3(-tx, -ty, tz),  Vec3(tx, -ty, tz),
        Vec3(tx, ty, tz),    Vec3(-tx, ty, tz)
    };

    std::vector<Vec3> vertices(8);
    for (int i = 0; i < 8; i++) {
        vertices[i] = aplicarRotacao(cubo.pos + locais[i], cubo.pos, cubo.rot);
    }

    // CORREÇÃO: Ordens dos vértices refeitas em sentido anti-horário
    std::vector<std::array<int,4>> faces = {
        {0, 3, 2, 1}, // Frente
        {4, 5, 6, 7}, // Trás
        {0, 1, 5, 4}, // Baixo
        {3, 7, 6, 2}, // Cima
        {0, 4, 7, 3}, // Esquerda
        {1, 2, 6, 5}  // Direita
    };

    uint32_t faceColor = obterCorPorMaterial(cubo.material);
    if (faceColor == 0xFFDDDDDD && cubo.cor != 0xFF0080FF) faceColor = cubo.cor;

    desenharFacesOrdenadas(pintor, vertices, faces, cam, w, h, faceColor, luz, cubos, cubo.material.albedoTex);
}

void desenharPlano3D(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, const LuzDirecional& luz, const std::vector<Cube>& cubos) {
    float tx = cubo.escala.x / 2.0f;
    float tz = cubo.escala.z / 2.0f;
    Vec3 locais[4] = {
        Vec3(-tx, 0.0f, -tz), Vec3(tx, 0.0f, -tz), 
        Vec3(tx, 0.0f, tz), Vec3(-tx, 0.0f, tz)
    };
    std::vector<Vec3> vertices(4);
    for (int i = 0; i < 4; i++) {
        vertices[i] = aplicarRotacao(cubo.pos + locais[i], cubo.pos, cubo.rot);
    }
    
    // CORREÇÃO: Agora o plano tem duas faces (Cima e Baixo) para nunca ficar invisível
    std::vector<std::array<int,4>> faces = {
        {0, 3, 2, 1}, // Face apontada para cima
        {0, 1, 2, 3}  // Face apontada para baixo
    };
    
    uint32_t faceColor = obterCorPorMaterial(cubo.material);
    if (faceColor == 0xFFDDDDDD && cubo.cor != 0xFF0080FF) faceColor = cubo.cor;
    desenharFacesOrdenadas(pintor, vertices, faces, cam, w, h, faceColor, luz, cubos, cubo.material.albedoTex);
}

// per-object light gizmo drawing removed

void desenharGizmoSetas(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, GizmoMode modeSelecionado) {
    float s = fmaxf(fmaxf(cubo.escala.x, cubo.escala.y), cubo.escala.z);
    // determine world size so gizmo appears roughly constant in screen pixels
    float base = worldUnitsForPixels(cam, cubo.pos, h, 80.0f);
    float tamanhoSeta = fmaxf(0.2f, base * 0.9f);
    float tamanhoCabeca = fmaxf(0.05f, base * 0.18f);
    float espessura = fmaxf(3.0f, base * 0.08f);

    uint32_t corX = modeSelecionado == GIZMO_MOVE_X ? 0xFFFFFF00 : COR_X;
    uint32_t corY = modeSelecionado == GIZMO_MOVE_Y ? 0xFFFFFF00 : COR_Y;
    uint32_t corZ = modeSelecionado == GIZMO_MOVE_Z ? 0xFFFFFF00 : COR_Z;

    Vec3 fimX = cubo.pos + Vec3(tamanhoSeta, 0, 0);
    Vec3 fimY = cubo.pos + Vec3(0, tamanhoSeta, 0);
    Vec3 fimZ = cubo.pos + Vec3(0, 0, tamanhoSeta);

    desenharLinha3DGrossa(pintor, cubo.pos, fimX, cam, w, h, corX, espessura);
    desenharLinha3DGrossa(pintor, cubo.pos, fimY, cam, w, h, corY, espessura);
    desenharLinha3DGrossa(pintor, cubo.pos, fimZ, cam, w, h, corZ, espessura);

    desenharLinha3DGrossa(pintor, fimX, fimX + Vec3(-tamanhoCabeca, tamanhoCabeca * 0.5f, 0), cam, w, h, corX, espessura);
    desenharLinha3DGrossa(pintor, fimX, fimX + Vec3(-tamanhoCabeca, -tamanhoCabeca * 0.5f, 0), cam, w, h, corX, espessura);
    desenharLinha3DGrossa(pintor, fimY, fimY + Vec3(tamanhoCabeca * 0.5f, -tamanhoCabeca, 0), cam, w, h, corY, espessura);
    desenharLinha3DGrossa(pintor, fimY, fimY + Vec3(-tamanhoCabeca * 0.5f, -tamanhoCabeca, 0), cam, w, h, corY, espessura);
    desenharLinha3DGrossa(pintor, fimZ, fimZ + Vec3(tamanhoCabeca * 0.5f, 0, -tamanhoCabeca), cam, w, h, corZ, espessura);
    desenharLinha3DGrossa(pintor, fimZ, fimZ + Vec3(-tamanhoCabeca * 0.5f, 0, -tamanhoCabeca), cam, w, h, corZ, espessura);
}

void desenharGizmoEscala(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, GizmoMode modeSelecionado) {
    float s = fmaxf(fmaxf(cubo.escala.x, cubo.escala.y), cubo.escala.z);
    float base = worldUnitsForPixels(cam, cubo.pos, h, 80.0f);
    float comprimento = fmaxf(0.2f, base * 0.9f);
    float espessura = fmaxf(4.0f, base * 0.1f);
    float tamanhoHandle = fmaxf(0.05f, base * 0.18f);

    uint32_t corX = modeSelecionado == GIZMO_SCALE_X ? 0xFFFFFF00 : COR_X;
    uint32_t corY = modeSelecionado == GIZMO_SCALE_Y ? 0xFFFFFF00 : COR_Y;
    uint32_t corZ = modeSelecionado == GIZMO_SCALE_Z ? 0xFFFFFF00 : COR_Z;

    Vec3 fimX = cubo.pos + Vec3(comprimento, 0, 0);
    Vec3 fimY = cubo.pos + Vec3(0, comprimento, 0);
    Vec3 fimZ = cubo.pos + Vec3(0, 0, comprimento);

    desenharLinha3DGrossa(pintor, cubo.pos, fimX, cam, w, h, corX, espessura);
    desenharLinha3DGrossa(pintor, cubo.pos, fimY, cam, w, h, corY, espessura);
    desenharLinha3DGrossa(pintor, cubo.pos, fimZ, cam, w, h, corZ, espessura);

    // Handles X
    desenharLinha3DGrossa(pintor, fimX + Vec3(0, tamanhoHandle, 0), fimX + Vec3(0, -tamanhoHandle, 0), cam, w, h, corX, espessura);
    desenharLinha3DGrossa(pintor, fimX + Vec3(0, 0, tamanhoHandle), fimX + Vec3(0, 0, -tamanhoHandle), cam, w, h, corX, espessura);

    // Handles Y
    desenharLinha3DGrossa(pintor, fimY + Vec3(tamanhoHandle, 0, 0), fimY + Vec3(-tamanhoHandle, 0, 0), cam, w, h, corY, espessura);
    desenharLinha3DGrossa(pintor, fimY + Vec3(0, 0, tamanhoHandle), fimY + Vec3(0, 0, -tamanhoHandle), cam, w, h, corY, espessura);

    // Handles Z
    desenharLinha3DGrossa(pintor, fimZ + Vec3(tamanhoHandle, 0, 0), fimZ + Vec3(-tamanhoHandle, 0, 0), cam, w, h, corZ, espessura);
    desenharLinha3DGrossa(pintor, fimZ + Vec3(0, tamanhoHandle, 0), fimZ + Vec3(0, -tamanhoHandle, 0), cam, w, h, corZ, espessura);
}

void desenharGizmoRotacao(SDL_Renderer* pintor, Cube& cubo, Camera& cam, int w, int h, GizmoMode modeSelecionado) {
    float s = fmaxf(fmaxf(cubo.escala.x, cubo.escala.y), cubo.escala.z);
    int numSegmentos = 24;
    float base = worldUnitsForPixels(cam, cubo.pos, h, 90.0f);
    float espessura = fmaxf(4.5f, base * 0.09f);

    uint32_t corX = modeSelecionado == GIZMO_ROT_X ? 0xFFFFFF00 : COR_X;
    uint32_t corY = modeSelecionado == GIZMO_ROT_Y ? 0xFFFFFF00 : COR_Y;
    uint32_t corZ = modeSelecionado == GIZMO_ROT_Z ? 0xFFFFFF00 : COR_Z;

    // Separe os arcos para não sobrepor: X (maior), Y (médio), Z (menor)
    // Make rotation gizmos separated and proportional on screen
    float raioX = base * 0.5f;
    float raioY = base * 0.9f;
    float raioZ = base * 0.7f;

    for (int i = 0; i < numSegmentos; i++) {
        float ang1 = (float)i / numSegmentos * 3.14159f * 2.0f;
        float ang2 = (float)(i + 1) / numSegmentos * 3.14159f * 2.0f;
        Vec3 p1 = cubo.pos + Vec3(0, cosf(ang1) * raioX, sinf(ang1) * raioX);
        Vec3 p2 = cubo.pos + Vec3(0, cosf(ang2) * raioX, sinf(ang2) * raioX);
        desenharLinha3DGrossa(pintor, p1, p2, cam, w, h, corX, espessura);
    }

    for (int i = 0; i < numSegmentos; i++) {
        float ang1 = (float)i / numSegmentos * 3.14159f * 2.0f;
        float ang2 = (float)(i + 1) / numSegmentos * 3.14159f * 2.0f;
        Vec3 p1 = cubo.pos + Vec3(cosf(ang1) * raioY, 0, sinf(ang1) * raioY);
        Vec3 p2 = cubo.pos + Vec3(cosf(ang2) * raioY, 0, sinf(ang2) * raioY);
        desenharLinha3DGrossa(pintor, p1, p2, cam, w, h, corY, espessura);
    }

    for (int i = 0; i < numSegmentos; i++) {
        float ang1 = (float)i / numSegmentos * 3.14159f * 2.0f;
        float ang2 = (float)(i + 1) / numSegmentos * 3.14159f * 2.0f;
        Vec3 p1 = cubo.pos + Vec3(cosf(ang1) * raioZ, sinf(ang1) * raioZ, 0);
        Vec3 p2 = cubo.pos + Vec3(cosf(ang2) * raioZ, sinf(ang2) * raioZ, 0);
        desenharLinha3DGrossa(pintor, p1, p2, cam, w, h, corZ, espessura);
    }
}

int verificarCliqueCubo(Vec3 mouseWorldPos, Cube& cubo, Camera& cam) {
    float tx = cubo.escala.x / 2.0f;
    float ty = cubo.escala.y / 2.0f;
    float tz = cubo.escala.z / 2.0f;
    return (mouseWorldPos.x >= cubo.pos.x - tx && mouseWorldPos.x <= cubo.pos.x + tx &&
            mouseWorldPos.y >= cubo.pos.y - ty && mouseWorldPos.y <= cubo.pos.y + ty &&
            mouseWorldPos.z >= cubo.pos.z - tz && mouseWorldPos.z <= cubo.pos.z + tz);
}

float calcularDeltaAoLongoDoEixo(ImVec2 mousePos, ImVec2 mouseAnterior, Vec3 eixoDir, Camera& cam, int w, int h, Vec3 eixoOrigem) {
    // Projeta o ponto de origem e o ponto final do eixo na tela
    Vec2 proj0 = projetarPonto3D(eixoOrigem, cam, w, h);
    Vec2 proj1 = projetarPonto3D(eixoOrigem + eixoDir, cam, w, h);
    
    // Calcula a direção do eixo na tela 2D
    Vec2 dirEixoScreen = {proj1.x - proj0.x, proj1.y - proj0.y};
    float lenEixo = sqrtf(dirEixoScreen.x * dirEixoScreen.x + dirEixoScreen.y * dirEixoScreen.y);
    
    if (lenEixo < 0.001f) return 0.0f; // Eixo muito pequeno na tela
    
    // Normaliza a direção do eixo
    dirEixoScreen.x /= lenEixo;
    dirEixoScreen.y /= lenEixo;
    
    // Delta do mouse em coordenadas de tela
    Vec2 deltaMouse = {mousePos.x - mouseAnterior.x, mousePos.y - mouseAnterior.y};
    
    // Projeta o delta do mouse na direção do eixo (dot product)
    float deltaAlongAxis = deltaMouse.x * dirEixoScreen.x + deltaMouse.y * dirEixoScreen.y;
    
    return deltaAlongAxis * 0.01f;
}

int detectarGizmo(ImVec2 mousePos, Cube& cubo, Camera& cam, int w, int h, GizmoMode& modoDetectado, GizmoMode modoAtivo) {
    if (modoAtivo == GIZMO_NONE) modoAtivo = GIZMO_MOVE_X;

    float s = fmaxf(fmaxf(cubo.escala.x, cubo.escala.y), cubo.escala.z);
    auto distancia = [&](Vec3 ponto) {
        Vec2 proj = projetarPonto3D(ponto, cam, w, h);
        return sqrtf((proj.x - mousePos.x) * (proj.x - mousePos.x) +
                     (proj.y - mousePos.y) * (proj.y - mousePos.y));
    };
    // detection radius in pixels: larger for better clickability on gizmo tips
    float desiredDetectPx = 45.0f;
    float worldDetect = worldUnitsForPixels(cam, cubo.pos, h, desiredDetectPx);
    const float raioDeteccao = fmaxf(12.0f, desiredDetectPx);
    if (gizmoEhMovimento(modoAtivo)) {
        // sample along all movement lines for consistent clicking
        float base = worldUnitsForPixels(cam, cubo.pos, h, 80.0f);
        float tamanhoSeta = fmaxf(0.2f, base * 0.9f);
        int samples = 32;
        float melhorDist = 1e30f;
        GizmoMode melhorModo = GIZMO_NONE;

        // Sample along X movement line
        for (int i = 0; i <= samples; i++) {
            float t = (float)i / (float)samples;
            Vec3 pX = cubo.pos + Vec3(tamanhoSeta * t, 0, 0);
            float d = distancia(pX);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_MOVE_X; }
        }

        // Sample along Y movement line
        for (int i = 0; i <= samples; i++) {
            float t = (float)i / (float)samples;
            Vec3 pY = cubo.pos + Vec3(0, tamanhoSeta * t, 0);
            float d = distancia(pY);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_MOVE_Y; }
        }

        // Sample along Z movement line
        for (int i = 0; i <= samples; i++) {
            float t = (float)i / (float)samples;
            Vec3 pZ = cubo.pos + Vec3(0, 0, tamanhoSeta * t);
            float d = distancia(pZ);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_MOVE_Z; }
        }

        if (melhorDist < raioDeteccao) {
            modoDetectado = melhorModo;
            return 1;
        }
        return 0;
    }

    if (gizmoEhEscala(modoAtivo)) {
        // sample along all scale lines for consistent clicking
        float base = worldUnitsForPixels(cam, cubo.pos, h, 80.0f);
        float comprimento = fmaxf(0.2f, base * 0.9f);
        int samples = 32;
        float melhorDist = 1e30f;
        GizmoMode melhorModo = GIZMO_NONE;

        // Sample along X scale line
        for (int i = 0; i <= samples; i++) {
            float t = (float)i / (float)samples;
            Vec3 pX = cubo.pos + Vec3(comprimento * t, 0, 0);
            float d = distancia(pX);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_SCALE_X; }
        }

        // Sample along Y scale line
        for (int i = 0; i <= samples; i++) {
            float t = (float)i / (float)samples;
            Vec3 pY = cubo.pos + Vec3(0, comprimento * t, 0);
            float d = distancia(pY);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_SCALE_Y; }
        }

        // Sample along Z scale line
        for (int i = 0; i <= samples; i++) {
            float t = (float)i / (float)samples;
            Vec3 pZ = cubo.pos + Vec3(0, 0, comprimento * t);
            float d = distancia(pZ);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_SCALE_Z; }
        }

        if (melhorDist < raioDeteccao) {
            modoDetectado = melhorModo;
            return 1;
        }
        return 0;
    }

    if (gizmoEhRotacao(modoAtivo)) {
        // sample along the arcs (screen-space tips) to allow clicking on arc endpoints
        float base = worldUnitsForPixels(cam, cubo.pos, h, 90.0f);
        float raioX = base * 0.5f;
        float raioY = base * 0.9f;
        float raioZ = base * 0.7f;
        int samples = 48;
        float melhorDist = 1e30f;
        GizmoMode melhorModo = GIZMO_NONE;

        for (int i = 0; i < samples; i++) {
            float ang = (float)i / (float)samples * 3.14159f * 2.0f;
            Vec3 pX = cubo.pos + Vec3(0, cosf(ang) * raioX, sinf(ang) * raioX);
            float d = distancia(pX);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_ROT_X; }

            Vec3 pY = cubo.pos + Vec3(cosf(ang) * raioY, 0, sinf(ang) * raioY);
            d = distancia(pY);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_ROT_Y; }

            Vec3 pZ = cubo.pos + Vec3(cosf(ang) * raioZ, sinf(ang) * raioZ, 0);
            d = distancia(pZ);
            if (d < melhorDist) { melhorDist = d; melhorModo = GIZMO_ROT_Z; }
        }

        if (melhorDist < raioDeteccao) {
            modoDetectado = melhorModo;
            return 1;
        }
        return 0;
    }

    return 0;
}

SDL_Texture* CriarTexturaGrade(SDL_Renderer* pintor, int largura, int altura) {
    SDL_Texture* tex = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, largura, altura);
    uint32_t* pixels = (uint32_t*)malloc(largura * altura * sizeof(uint32_t));
    float* heights = (float*)malloc(largura * altura * sizeof(float));

    const int square = 16;
    // first pass: build heightmap per pixel (beveled squares)
    for (int y = 0; y < altura; y++) {
        for (int x = 0; x < largura; x++) {
            int sx = x / square;
            int sy = y / square;
            int xadrez = (sx + sy) & 1;

            float lx = (x % square) / (float)square - 0.5f;
            float ly = (y % square) / (float)square - 0.5f;
            float dist = sqrtf(lx*lx + ly*ly) / 0.7071f; // normalize by max dist (~sqrt(0.5))
            if (dist > 1.0f) dist = 1.0f;

            float base = xadrez ? 0.25f : 0.6f; // dark vs light base height
            float bevel = (1.0f - dist) * 0.35f; // raised toward square center
            heights[y * largura + x] = base + bevel;
        }
    }

    // lighting parameters
    const float lx = 0.5f, ly = 0.5f, lz = 1.0f;
    float llen = sqrtf(lx*lx + ly*ly + lz*lz);
    const float ldx = lx/llen, ldy = ly/llen, ldz = lz/llen;

    // second pass: compute normals and shaded colors (neon colors)
    for (int y = 0; y < altura; y++) {
        for (int x = 0; x < largura; x++) {
            float hC = heights[y * largura + x];
            float hL = (x > 0) ? heights[y * largura + (x-1)] : hC;
            float hR = (x < largura-1) ? heights[y * largura + (x+1)] : hC;
            float hU = (y > 0) ? heights[(y-1) * largura + x] : hC;
            float hD = (y < altura-1) ? heights[(y+1) * largura + x] : hC;

            float nx = hL - hR;
            float ny = hU - hD;
            float nz = 1.0f;
            float invLen = 1.0f / sqrtf(nx*nx + ny*ny + nz*nz);
            nx *= invLen; ny *= invLen; nz *= invLen;

            float diff = nx*ldx + ny*ldy + nz*ldz;
            if (diff < 0.0f) diff = 0.0f;
            float shade = 0.25f + 0.75f * diff; // ambient + diffuse

            int sx = x / square;
            int sy = y / square;
            int xadrez = (sx + sy) & 1;

            // neon colors: blue and purple
            float baseR = xadrez ? 200.0f : 0.0f;   // purple vs blue
            float baseG = xadrez ? 0.0f : 180.0f;
            float baseB = xadrez ? 255.0f : 255.0f;

            uint8_t r = (uint8_t)fminf(255.0f, baseR * shade);
            uint8_t g = (uint8_t)fminf(255.0f, baseG * shade);
            uint8_t b = (uint8_t)fminf(255.0f, baseB * shade);

            pixels[y * largura + x] = (255u << 24) | (r << 16) | (g << 8) | b;
        }
    }

    SDL_UpdateTexture(tex, NULL, pixels, largura * sizeof(uint32_t));
    free(pixels);
    free(heights);
    return tex;
}

SDL_Texture* CriarTexturaFundo(SDL_Renderer* pintor, int largura, int altura) {
    SDL_Texture* tex = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, largura, altura);
    uint32_t* pixels = (uint32_t*)malloc(largura * altura * sizeof(uint32_t));

    const float cx = largura / 2.0f;
    const float cy = altura / 2.0f;
    const float maxd = sqrtf(cx*cx + cy*cy);

    for (int y = 0; y < altura; y++) {
        for (int x = 0; x < largura; x++) {
            float dx = x - cx;
            float dy = y - cy;
            float d = sqrtf(dx*dx + dy*dy) / maxd; // 0..1
            if (d > 1.0f) d = 1.0f;

            // lerp colors from center (lighter) to edges (darker)
            uint8_t r = (uint8_t)((1.0f - d) * 180 + d * 30); // center ~180 -> edge ~30
            uint8_t g = (uint8_t)((1.0f - d) * 40 + d * 0);
            uint8_t b = (uint8_t)((1.0f - d) * 220 + d * 100);

            pixels[y * largura + x] = (255u << 24) | (r << 16) | (g << 8) | b;
        }
    }

    SDL_UpdateTexture(tex, NULL, pixels, largura * sizeof(uint32_t));
    free(pixels);
    return tex;
}

SDL_Texture* CriarTextura3D(SDL_Renderer* pintor, int largura, int altura) {
    SDL_Texture* tex = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, largura, altura);
    uint32_t* pixels = (uint32_t*)malloc(largura * altura * sizeof(uint32_t));

    // procedural heightmap using sine waves
    const float fx = 0.07f;
    const float fy = 0.09f;
    for (int y = 0; y < altura; y++) {
        for (int x = 0; x < largura; x++) {
            float hx = 0.5f + 0.5f * sinf((float)x * fx) * cosf((float)y * fy);
            float hy = 0.5f + 0.5f * sinf((float)y * fy * 1.5f) * cosf((float)x * fx * 1.2f);
            float height = (hx + hy) * 0.5f; // 0..1

            // approximate normal via derivatives
            float hL = 0.5f + 0.5f * sinf((float)(x-1) * fx) * cosf((float)y * fy);
            float hR = 0.5f + 0.5f * sinf((float)(x+1) * fx) * cosf((float)y * fy);
            float hU = 0.5f + 0.5f * sinf((float)x * fx) * cosf((float)(y-1) * fy);
            float hD = 0.5f + 0.5f * sinf((float)x * fx) * cosf((float)(y+1) * fy);

            float nx = hL - hR;
            float ny = hU - hD;
            float nz = 1.0f;
            float invLen = 1.0f / sqrtf(nx*nx + ny*ny + nz*nz);
            nx *= invLen; ny *= invLen; nz *= invLen;

            // lighting
            const float lx = 0.5f, ly = 0.5f, lz = 1.0f;
            float lLen = sqrtf(lx*lx + ly*ly + lz*lz);
            float ldx = lx / lLen, ldy = ly / lLen, ldz = lz / lLen;
            float diff = nx*ldx + ny*ldy + nz*ldz;
            if (diff < 0.0f) diff = 0.0f;

            float shade = 0.3f + 0.7f * diff; // ambient + diffuse

            // create a darkening overlay: alpha = (1 - shade) * 220
            uint8_t alpha = (uint8_t)(fminf(1.0f, fmaxf(0.0f, 1.0f - shade)) * 220.0f);
            pixels[y * largura + x] = ((uint32_t)alpha << 24) | 0x000000;
        }
    }
    SDL_UpdateTexture(tex, NULL, pixels, largura * sizeof(uint32_t));
    free(pixels);
    return tex;
}

SDL_Texture* CriarIconeBotao(SDL_Renderer* pintor, int largura, int altura, uint32_t corFundo, uint32_t corDetalhe, bool viewportIcon) {
    SDL_Texture* tex = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, largura, altura);
    if (!tex) return NULL;
    uint32_t* pixels = (uint32_t*)malloc(largura * altura * sizeof(uint32_t));
    if (!pixels) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    for (int y = 0; y < altura; y++) {
        for (int x = 0; x < largura; x++) {
            pixels[y * largura + x] = 0x00000000u;
        }
    }

    auto putPixel = [&](int px, int py, uint32_t color) {
        if (px >= 0 && px < largura && py >= 0 && py < altura) {
            pixels[py * largura + px] = color;
        }
    };

    uint32_t detalhe = corDetalhe;

    if (viewportIcon) {
        for (int x = 10; x < largura - 10; x++) {
            putPixel(x, 10, detalhe);
            putPixel(x, altura - 11, detalhe);
        }
        for (int y = 10; y < altura - 10; y++) {
            putPixel(10, y, detalhe);
            putPixel(largura - 11, y, detalhe);
        }
        int centerY = altura / 2;
        for (int x = 14; x < largura - 14; x++) putPixel(x, centerY, detalhe);
        int centerX = largura / 2;
        for (int y = 14; y < altura - 10; y++) putPixel(centerX, y, detalhe);
    } else {
        // Draw three filled blocks centered in the icon (avoid clipping)
        int blockSize = 10;
        int spacing = 6;
        int totalWidth = blockSize * 3 + spacing * 2;
        int startX = (largura - totalWidth) / 2;
        int y0 = (altura - blockSize) / 2;
        for (int i = 0; i < 3; i++) {
            int x0 = startX + i * (blockSize + spacing);
            for (int yy = y0; yy < y0 + blockSize; yy++) {
                for (int xx = x0; xx < x0 + blockSize; xx++) {
                    putPixel(xx, yy, detalhe);
                }
            }
        }
    }

    SDL_UpdateTexture(tex, NULL, pixels, largura * sizeof(uint32_t));
    free(pixels);
    return tex;
}

SDL_Texture* CriarIconeViewport(SDL_Renderer* pintor, int largura, int altura) {
    return CriarIconeBotao(pintor, largura, altura, 0xFF404040, 0xFFE0E0E0, true);
}

SDL_Texture* CriarIconeInspetor(SDL_Renderer* pintor, int largura, int altura) {
    SDL_Texture* tex = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, largura, altura);
    if (!tex) return NULL;
    uint32_t* pixels = (uint32_t*)malloc(largura * altura * sizeof(uint32_t));
    if (!pixels) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    for (int y = 0; y < altura; y++) {
        for (int x = 0; x < largura; x++) {
            pixels[y * largura + x] = 0x00000000u;
        }
    }

    auto putPixel = [&](int px, int py, uint32_t color) {
        if (px >= 0 && px < largura && py >= 0 && py < altura) {
            pixels[py * largura + px] = color;
        }
    };

    uint32_t detalhe = 0xFFE0E0E0;
    
    // Desenha um ícone de inspetor: quadrados aninhados com um símbolo de lupa/informação
    // Quadrado externo
    for (int x = 8; x < largura - 8; x++) {
        putPixel(x, 8, detalhe);
        putPixel(x, altura - 9, detalhe);
    }
    for (int y = 8; y < altura - 8; y++) {
        putPixel(8, y, detalhe);
        putPixel(largura - 9, y, detalhe);
    }
    
    // Pequeno "i" de informação no centro
    int centerX = largura / 2;
    int centerY = altura / 2;
    putPixel(centerX, centerY - 4, detalhe);  // Ponto do "i"
    for (int dy = -2; dy <= 3; dy++) {
        putPixel(centerX, centerY + dy, detalhe);  // Linha do "i"
    }

    SDL_UpdateTexture(tex, NULL, pixels, largura * sizeof(uint32_t));
    free(pixels);
    return tex;
}

SDL_Texture* CriarIconeBlocos(SDL_Renderer* pintor, int largura, int altura) {
    return CriarIconeBotao(pintor, largura, altura, 0xFF404040, 0xFFFFFFFF, false);
}

void DesenharTelaDoEditor(SDL_Renderer* pintor, int telaW, int telaH, SDL_Texture* texturaFundo, SDL_Texture* texturaDoJogo, SDL_Texture* textura3D) {
    // Desenhar fundo roxo bonito usando a textura de fundo
    if (texturaFundo) {
        SDL_RenderCopy(pintor, texturaFundo, NULL, NULL);
    } else {
        SDL_SetRenderDrawColor(pintor, 100, 60, 130, 255);
        SDL_Rect r = {0, 0, telaW, telaH};
        SDL_RenderFillRect(pintor, &r);
    }
}

void DesenharViewportEngine(SDL_Renderer* pintor, Camera& cam, std::vector<Cube>& cubos, ImVec2 pos, ImVec2 size, ImVec2 mousePos, int mouseClaqueouAqui, GizmoState& gizmo, const LuzDirecional& luz, bool luzVisivel, bool luzSelecionada) {
    if (size.x <= 0 || size.y <= 0) return;

    int w = (int)size.x;
    int h = (int)size.y;
    int numCubos = (int)cubos.size();

    // Configura o viewport para delimitar TODA renderização (fundo e conteúdo 3D) 
    // apenas dentro da área útil da janela "Engine Viewport" do ImGui
    SDL_Rect viewport = { (int)pos.x, (int)pos.y, w, h };
    SDL_RenderSetViewport(pintor, &viewport);

    // Renderiza o fundo roxo claro (céu) - será desenhado APENAS dentro do viewport configurado acima
    SDL_SetRenderDrawColor(pintor, 200, 150, 255, 255);
    SDL_Rect skyRect = viewport;
    skyRect.x = 0;  // Coordenadas locais relativas ao viewport
    skyRect.y = 0;
    SDL_RenderFillRect(pintor, &skyRect);

    // Desenha grade do chão
    SDL_SetRenderDrawColor(pintor, 100, 100, 150, 100);
    for (int i = -10; i <= 10; i++) {
        desenharLinha3D(pintor, Vec3(i, 0, -10), Vec3(i, 0, 10), cam, w, h, 0xFF646496);
        desenharLinha3D(pintor, Vec3(-10, 0, i), Vec3(10, 0, i), cam, w, h, 0xFF646496);
    }

    // Desenha cubos
    // Ordenar cubos por profundidade (distância do centro) para desenhar
    // primeiro os mais distantes e depois os mais próximos (painter's algorithm)
    struct IdxDepth { int idx; float depth; } ids[100];
    for (int i = 0; i < numCubos; i++) {
        ids[i].idx = i;
        ids[i].depth = (cubos[i].pos - cam.pos).length();
    }
    std::sort(ids, ids + numCubos, [](const IdxDepth &a, const IdxDepth &b) { return a.depth > b.depth; });

    for (int k = 0; k < numCubos; k++) {
        int i = ids[k].idx;
        
        // Desenha o objeto baseado no seu tipo
        switch(cubos[i].tipo) {
            case BLOCO_CUBO:
                desenharCubo3D(pintor, cubos[i], cam, w, h, luz, cubos);
                break;
            case BLOCO_PLANO:
                desenharPlano3D(pintor, cubos[i], cam, w, h, luz, cubos);
                break;
            case BLOCO_CONE:
                desenharCone3D(pintor, cubos[i], cam, w, h, luz, cubos);
                break;
            case BLOCO_ESFERA:
                desenharEsfera3D(pintor, cubos[i], cam, w, h, luz, cubos);
                break;
        }

        // Desenha gizmos do cubo selecionado
        if (cubos[i].selecionado) {
            int modoAtivo = gizmo.cuboSelecionado == i ? gizmo.modo : GIZMO_MOVE_X;
            if (modoAtivo == GIZMO_NONE) modoAtivo = GIZMO_MOVE_X;
            if (gizmoEhMovimento(modoAtivo)) {
                desenharGizmoSetas(pintor, cubos[i], cam, w, h, (GizmoMode)modoAtivo);
            } else if (gizmoEhEscala(modoAtivo)) {
                desenharGizmoEscala(pintor, cubos[i], cam, w, h, (GizmoMode)modoAtivo);
            } else if (gizmoEhRotacao(modoAtivo)) {
                desenharGizmoRotacao(pintor, cubos[i], cam, w, h, (GizmoMode)modoAtivo);
            }
        }
    }
    if (luzVisivel) {
        desenharLuzHandle(pintor, cam, luz, w, h, luzSelecionada);
    }

    // Detecta clique do mouse no viewport
    if (mouseClaqueouAqui) {
        GizmoMode modoDetectado = GIZMO_NONE;
        bool foiGizmo = false;
        
        for (int i = 0; i < numCubos; i++) {
            if (cubos[i].selecionado && detectarGizmo(mousePos, cubos[i], cam, w, h, modoDetectado, (GizmoMode)gizmo.modo)) {
                gizmo.modo = modoDetectado;
                gizmo.cuboSelecionado = i;
                gizmo.arrastando = true;
                gizmo.mouseAnterior = mousePos;
                foiGizmo = true;
                break;
            }
        }
        
        if (!foiGizmo) {
            int selecionado = -1;
            for (int i = 0; i < (int)cubos.size(); i++) {
                Vec2 projCubo = projetarPonto3D(cubos[i].pos, cam, w, h);
                float distMouse = sqrtf((projCubo.x - mousePos.x) * (projCubo.x - mousePos.x) + 
                                        (projCubo.y - mousePos.y) * (projCubo.y - mousePos.y));
                if (distMouse < 40) {
                    selecionado = i;
                    break;
                }
            }
            for (int i = 0; i < numCubos; i++) {
                cubos[i].selecionado = (i == selecionado) ? 1 : 0;
            }
            if (selecionado != gizmo.cuboSelecionado) {
                gizmo.modo = GIZMO_NONE;
            }
            gizmo.cuboSelecionado = selecionado;
            gizmo.arrastando = false;
        }
    }
    
    if (gizmo.arrastando && gizmo.cuboSelecionado >= 0 && gizmo.cuboSelecionado < numCubos && gizmo.modo != GIZMO_NONE) {
        Cube& cubo = cubos[gizmo.cuboSelecionado];
        float base = worldUnitsForPixels(cam, cubo.pos, h, 80.0f);
        
        switch(gizmo.modo) {
            case GIZMO_MOVE_X: {
                float delta = calcularDeltaAoLongoDoEixo(mousePos, gizmo.mouseAnterior, Vec3(base * 0.9f, 0, 0), cam, w, h, cubo.pos);
                cubo.pos.x += delta;
                break;
            }
            case GIZMO_MOVE_Y: {
                float delta = calcularDeltaAoLongoDoEixo(mousePos, gizmo.mouseAnterior, Vec3(0, base * 0.9f, 0), cam, w, h, cubo.pos);
                cubo.pos.y += delta;
                break;
            }
            case GIZMO_MOVE_Z: {
                float delta = calcularDeltaAoLongoDoEixo(mousePos, gizmo.mouseAnterior, Vec3(0, 0, base * 0.9f), cam, w, h, cubo.pos);
                cubo.pos.z += delta;
                break;
            }
            case GIZMO_SCALE_X: {
                float delta = calcularDeltaAoLongoDoEixo(mousePos, gizmo.mouseAnterior, Vec3(base * 0.9f, 0, 0), cam, w, h, cubo.pos);
                cubo.escala.x += delta;
                if (cubo.escala.x < 0.1f) cubo.escala.x = 0.1f;
                break;
            }
            case GIZMO_SCALE_Y: {
                float delta = calcularDeltaAoLongoDoEixo(mousePos, gizmo.mouseAnterior, Vec3(0, base * 0.9f, 0), cam, w, h, cubo.pos);
                cubo.escala.y += delta;
                if (cubo.escala.y < 0.1f) cubo.escala.y = 0.1f;
                break;
            }
            case GIZMO_SCALE_Z: {
                float delta = calcularDeltaAoLongoDoEixo(mousePos, gizmo.mouseAnterior, Vec3(0, 0, base * 0.9f), cam, w, h, cubo.pos);
                cubo.escala.z += delta;
                if (cubo.escala.z < 0.1f) cubo.escala.z = 0.1f;
                break;
            }
            case GIZMO_ROT_X: {
                float deltaMouse = (mousePos.x - gizmo.mouseAnterior.x + gizmo.mouseAnterior.y - mousePos.y) * 0.01f;
                cubo.rot.x -= deltaMouse * 24.0f;
                break;
            }
            case GIZMO_ROT_Y: {
                float deltaMouse = (mousePos.x - gizmo.mouseAnterior.x + gizmo.mouseAnterior.y - mousePos.y) * 0.01f;
                cubo.rot.y += deltaMouse * 24.0f;
                break;
            }
            case GIZMO_ROT_Z: {
                float deltaMouse = (mousePos.x - gizmo.mouseAnterior.x + gizmo.mouseAnterior.y - mousePos.y) * 0.01f;
                cubo.rot.z -= deltaMouse * 24.0f;
                break;
            }
            default: break;
        }
        gizmo.mouseAnterior = mousePos;
    }

    // Reseta o viewport IMEDIATAMENTE após o desenho 3D ser concluído
    // Isso garante que a renderização dos painéis do ImGui não seja afetada
    SDL_RenderSetViewport(pintor, NULL);
}

int main()
{
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        return -1;
    }

    SDL_Window* janela = SDL_CreateWindow("HollowEngine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1080, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (janela == NULL) {
        SDL_Quit();
        return -1;
    }

    SDL_Renderer* pintor = SDL_CreateRenderer(janela, -1, SDL_RENDERER_ACCELERATED);
    if (pintor == NULL) {
        SDL_DestroyWindow(janela);
        SDL_Quit();
        return -1;
    }

    const int texW = 1080;
    const int texH = 720;

    SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_BLEND);
    SDL_Texture* texturaFundo = CriarTexturaFundo(pintor, texW, texH);
    SDL_Texture* iconViewport = CriarIconeViewport(pintor, 72, 24);
    SDL_Texture* iconBlocos = CriarIconeBlocos(pintor, 72, 24);
    SDL_Texture* iconInspetor = CriarIconeInspetor(pintor, 72, 24);

    SDL_Event evento;
    int is_fullscreen = 0;

    // Sistema 3D
    Camera camera;
    std::vector<Cube> cubos;
    
    GizmoState gizmo = { GIZMO_NONE, -1, {0, 0}, false };
    // Luz da cena (direcional)
    LuzDirecional sceneLuz;
    bool sceneLuzVisivel = false;
    
    // Interface
    bool showObjectPanel = false;
    bool showViewport = false;
    bool showPropriedades = false;
    char nomeNovoCubo[32] = "Cubo";
    float posNovoCubo[3] = {0, 1, 0};
    float tamNovoCubo = 1.0f;
    int tipoNovoBloco = BLOCO_CUBO;

    const uint8_t* teclasAtivas = SDL_GetKeyboardState(NULL);
    bool viewportFocado = false;
    ImVec2 ultimoMouseViewport(0, 0);
    int mouseClaqueouViewport = 0;
    bool mouseEsquerdoPressionado = false;

    // ImGui initialization
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL2_InitForSDLRenderer(janela, pintor)) {
        SDL_DestroyTexture(texturaFundo);
        SDL_DestroyTexture(iconViewport);
        SDL_DestroyTexture(iconBlocos);
        SDL_DestroyTexture(iconInspetor);
        SDL_DestroyRenderer(pintor);
        SDL_DestroyWindow(janela);
        SDL_Quit();
        return -1;
    }
    if (!ImGui_ImplSDLRenderer2_Init(pintor)) {
        ImGui_ImplSDL2_Shutdown();
        SDL_DestroyTexture(texturaFundo);
        SDL_DestroyTexture(iconViewport);
        SDL_DestroyTexture(iconBlocos);
        SDL_DestroyTexture(iconInspetor);
        SDL_DestroyRenderer(pintor);
        SDL_DestroyWindow(janela);
        SDL_Quit();
        return -1;
    }

    bool lightArrastando = false;
    bool lightSelecionado = false;
    while (jogo_rodando) {
        mouseClaqueouViewport = 0;
        
        while (SDL_PollEvent(&evento)) {
            ImGui_ImplSDL2_ProcessEvent(&evento);
            if (evento.type == SDL_QUIT) {
                jogo_rodando = 0;
            } else if (evento.type == SDL_KEYDOWN) {
                if (evento.key.keysym.sym == SDLK_F11) {
                    if (!is_fullscreen) {
                        SDL_SetWindowFullscreen(janela, SDL_WINDOW_FULLSCREEN_DESKTOP);
                        is_fullscreen = 1;
                    } else {
                        SDL_SetWindowFullscreen(janela, 0);
                        is_fullscreen = 0;
                    }
                }
            } else if (evento.type == SDL_MOUSEBUTTONDOWN) {
                if (evento.button.button == SDL_BUTTON_LEFT) {
                    mouseEsquerdoPressionado = true;
                }
            } else if (evento.type == SDL_MOUSEBUTTONUP) {
                if (evento.button.button == SDL_BUTTON_LEFT) {
                    mouseEsquerdoPressionado = false;
                    gizmo.arrastando = false;
                    lightArrastando = false;
                }
            } else if (evento.type == SDL_MOUSEMOTION) {
                if (viewportFocado && lightArrastando) {
                    float base = worldUnitsForPixels(camera, sceneLuz.posicao, texH, 100.0f);
                    Vec3 right = camera.front.cross(camera.up).normalize();
                    Vec3 novoUp = right.cross(camera.front).normalize();
                    sceneLuz.posicao = sceneLuz.posicao + right * (evento.motion.xrel * 0.01f * base) - novoUp * (evento.motion.yrel * 0.01f * base);
                } else if (viewportFocado && (teclasAtivas[SDL_SCANCODE_LSHIFT] || teclasAtivas[SDL_SCANCODE_RSHIFT])) {
                    float sens = 0.3f;
                    camera.yaw += evento.motion.xrel * sens;
                    camera.pitch -= evento.motion.yrel * sens;
                    if (camera.pitch > 89.0f) camera.pitch = 89.0f;
                    if (camera.pitch < -89.0f) camera.pitch = -89.0f;
                    camera.atualizarDirecao();
                }
            }
        }

        teclasAtivas = SDL_GetKeyboardState(NULL);
        
        // Movimento da câmera com WASD
        if (viewportFocado) {
            float velocidade = 0.1f;
            if (teclasAtivas[SDL_SCANCODE_W]) {
                camera.pos = camera.pos + camera.front * velocidade;
            }
            if (teclasAtivas[SDL_SCANCODE_S]) {
                camera.pos = camera.pos - camera.front * velocidade;
            }
            if (teclasAtivas[SDL_SCANCODE_A]) {
                Vec3 right = camera.front.cross(camera.up).normalize();
                camera.pos = camera.pos - right * velocidade;
            }
            if (teclasAtivas[SDL_SCANCODE_D]) {
                Vec3 right = camera.front.cross(camera.up).normalize();
                camera.pos = camera.pos + right * velocidade;
            }
            if (teclasAtivas[SDL_SCANCODE_SPACE]) {
                camera.pos = camera.pos + camera.up * velocidade;
            }
            if (teclasAtivas[SDL_SCANCODE_LCTRL] || teclasAtivas[SDL_SCANCODE_RCTRL]) {
                camera.pos = camera.pos - camera.up * velocidade;
            }
        }

        int curW = texW, curH = texH;
        SDL_GetWindowSize(janela, &curW, &curH);

        SDL_SetRenderDrawColor(pintor, 0, 0, 0, 255);
        SDL_RenderClear(pintor);
        DesenharTelaDoEditor(pintor, curW, curH, texturaFundo, NULL, NULL);

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Top bar: fixed at top with two textured buttons (Viewport, Blocos, Inspetor)
        float barHeight = 36.0f;
        float viewportButtonWidth = 80.0f;
        float blocosButtonWidth = viewportButtonWidth; // same width as viewport
        float inspetorButtonWidth = viewportButtonWidth;
        float barWidth = viewportButtonWidth + blocosButtonWidth + inspetorButtonWidth + 12.0f; // three buttons with padding
        float barX = 0.0f;
        ImGui::SetNextWindowPos(ImVec2(barX, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(barWidth, barHeight), ImGuiCond_Always);
        ImGuiWindowFlags topBarFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0)); // ensure no border drawn
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0)); // remove extra padding/frame
        ImGui::Begin("HollowEngine UI", NULL, topBarFlags);

        ImGui::SetCursorPosX(4.0f);
        ImGui::SetCursorPosY(6.0f);
        if (ImGui::ImageButton("viewport_btn", (ImTextureID)iconViewport, ImVec2(viewportButtonWidth - 4.0f, 24.0f), ImVec2(0, 0), ImVec2(1, 1))) {
            showViewport = !showViewport;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Viewport");
        ImGui::SameLine();
        if (ImGui::ImageButton("blocos_btn", (ImTextureID)iconBlocos, ImVec2(blocosButtonWidth - 4.0f, 24.0f), ImVec2(0, 0), ImVec2(1, 1))) {
            showObjectPanel = !showObjectPanel;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blocos");
        ImGui::SameLine();
        if (ImGui::ImageButton("inspetor_btn", (ImTextureID)iconInspetor, ImVec2(inspetorButtonWidth - 4.0f, 24.0f), ImVec2(0, 0), ImVec2(1, 1))) {
            showPropriedades = !showPropriedades;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Propriedades");

        ImGui::End();
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar();

        // Object Creation Panel
        if (showObjectPanel) {
            ImGui::SetNextWindowPos(ImVec2(900, 50), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
            ImGui::Begin("Criar Blocos", NULL);
            
            ImGui::Text("Adicionar novo objeto:");
            ImGui::Separator();
            ImGui::InputFloat3("Posição##cubo", posNovoCubo);
            ImGui::InputFloat("Tamanho", &tamNovoCubo);
            if (tamNovoCubo < 0.1f) tamNovoCubo = 0.1f;

            const char* tiposBloco[] = {"Cubo", "Plano", "Cone", "Esfera"};
            ImGui::Combo("Tipo de bloco", &tipoNovoBloco, tiposBloco, IM_ARRAYSIZE(tiposBloco));
            
            if (ImGui::Button("Criar Objeto", ImVec2(-1, 0))) {
                Cube novoCubo(Vec3(posNovoCubo[0], posNovoCubo[1], posNovoCubo[2]), tamNovoCubo, tipoNovoBloco);
                novoCubo.cor = 0xFF808080 | ((rand() & 0xFF) << 24);
                cubos.push_back(novoCubo);
            }

            ImGui::Separator();
            ImGui::Text("Luz da cena:");
            if (!sceneLuzVisivel) {
                if (ImGui::Button("Adicionar luz", ImVec2(-1, 0))) {
                    sceneLuzVisivel = true;
                    sceneLuz.posicao = Vec3(0.0f, 3.0f, 0.0f);
                    sceneLuz.intensidade = 1.0f;
                }
            } else {
                if (ImGui::Button("Reiniciar luz", ImVec2(-1, 0))) {
                    sceneLuz.posicao = Vec3(0.0f, 3.0f, 0.0f);
                    sceneLuz.intensidade = 1.0f;
                }
                ImGui::TextWrapped("Luz ativa na viewport. Clique e arraste o bloco para mover.");
            }

            ImGui::Separator();
            ImGui::Text("Blocos: %d", (int)cubos.size());
            
            ImGui::Separator();
            ImGui::Text("Selecionado:");
            if (gizmo.cuboSelecionado >= 0 && gizmo.cuboSelecionado < (int)cubos.size()) {
                Cube& sel = cubos[gizmo.cuboSelecionado];
                const char* nomeTiposBloco[] = {"Cubo", "Plano", "Cone", "Esfera"};
                const char* tipoNome = "Desconhecido";
                if (sel.tipo >= BLOCO_CUBO && sel.tipo <= BLOCO_ESFERA) {
                    tipoNome = nomeTiposBloco[sel.tipo];
                }
                ImGui::Text("Tipo: %s", tipoNome);
                ImGui::Text("Pos: (%.2f, %.2f, %.2f)", sel.pos.x, sel.pos.y, sel.pos.z);
                ImGui::Text("Tam: (%.2f, %.2f, %.2f)", sel.escala.x, sel.escala.y, sel.escala.z);
                ImGui::Text("Rot: (%.2f, %.2f, %.2f)", sel.rot.x, sel.rot.y, sel.rot.z);
                
                if (ImGui::Button("Deletar Selecionado", ImVec2(-1, 0))) {
                    cubos.erase(cubos.begin() + gizmo.cuboSelecionado);
                    gizmo.cuboSelecionado = -1;
                }
            } else {
                ImGui::Text("Nenhum bloco selecionado");
            }
            
            ImGui::End();
        }

        // Propriedades (Inspector / Materiais / Iluminação)
        if (showPropriedades) {
            ImGui::SetNextWindowPos(ImVec2(620, 50), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(260, 400), ImGuiCond_FirstUseEver);
            ImGui::Begin("Propriedades", NULL);

            if (ImGui::BeginTabBar("PropriedadesTabs")) {
                    // --- Inspetor ---
                    if (ImGui::BeginTabItem("Inspetor")) {
                        if (gizmo.cuboSelecionado < 0 || gizmo.cuboSelecionado >= (int)cubos.size()) {
                            ImGui::TextWrapped("Selecione um objeto na Viewport para editar suas propriedades.");
                        } else {
                            Cube& obj = cubos[gizmo.cuboSelecionado];
                            ImGui::DragFloat3("Posicao", &obj.pos.x, 0.05f);
                            ImGui::DragFloat3("Rotacao", &obj.rot.x, 0.05f);
                            ImGui::DragFloat3("Escala", &obj.escala.x, 0.05f);
                        }
                        ImGui::EndTabItem();
                    }

                    // --- Materiais ---
                    if (ImGui::BeginTabItem("Materiais")) {
                        if (gizmo.cuboSelecionado < 0 || gizmo.cuboSelecionado >= (int)cubos.size()) {
                            ImGui::TextWrapped("Selecione um objeto na Viewport para editar suas propriedades.");
                        } else {
                            Cube& obj = cubos[gizmo.cuboSelecionado];
                            // Sample materials + Personalizado
                            static const char* exemplos[] = {"Tijolo","Madeira","Pedra","Grama","Neon","Metal","Personalizado"};
                            static int exemploSelecionado = 0;
                            if (ImGui::Combo("Materiais Exemplo", &exemploSelecionado, exemplos, IM_ARRAYSIZE(exemplos))) {
                                const char* escolha = exemplos[exemploSelecionado];
                                if (strcmp(escolha, "Tijolo") == 0) {
                                    obj.material.albedo = "tijolo_albedo.png";
                                    obj.material.normal = "tijolo_normal.png";
                                    obj.material.roughness = "tijolo_roughness.png";
                                } else if (strcmp(escolha, "Madeira") == 0) {
                                    obj.material.albedo = "madeira_albedo.png";
                                    obj.material.normal = "madeira_normal.png";
                                    obj.material.roughness = "madeira_roughness.png";
                                } else if (strcmp(escolha, "Pedra") == 0) {
                                    obj.material.albedo = "pedra_albedo.png";
                                    obj.material.normal = "pedra_normal.png";
                                    obj.material.roughness = "pedra_roughness.png";
                                } else if (strcmp(escolha, "Grama") == 0) {
                                    obj.material.albedo = "grama_albedo.png";
                                    obj.material.normal = "grama_normal.png";
                                    obj.material.roughness = "grama_roughness.png";
                                } else if (strcmp(escolha, "Neon") == 0) {
                                    obj.material.albedo = "neon_albedo.png";
                                    obj.material.normal = "neon_normal.png";
                                    obj.material.roughness = "neon_roughness.png";
                                } else if (strcmp(escolha, "Metal") == 0) {
                                    obj.material.albedo = "metal_albedo.png";
                                    obj.material.normal = "metal_normal.png";
                                    obj.material.roughness = "metal_roughness.png";
                                }
                            }

                            // Editable text fields for material paths/names
                            char bufAlbedo[256];
                            char bufNormal[256];
                            char bufRough[256];
                            strncpy(bufAlbedo, obj.material.albedo.c_str(), sizeof(bufAlbedo)-1); bufAlbedo[255] = '\0';
                            strncpy(bufNormal, obj.material.normal.c_str(), sizeof(bufNormal)-1); bufNormal[255] = '\0';
                            strncpy(bufRough, obj.material.roughness.c_str(), sizeof(bufRough)-1); bufRough[255] = '\0';

                            ImGui::TextWrapped("Edite os caminhos das texturas abaixo:");
                            if (ImGui::InputText("Albedo##mat", bufAlbedo, sizeof(bufAlbedo))) {
                                obj.material.albedo = std::string(bufAlbedo);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("...##albedo")) {
                                std::string selecionado = abrirSeletorArquivo("Selecionar Textura Albedo");
                                if (!selecionado.empty()) {
                                    obj.material.albedo = selecionado;
                                    liberarTextura(obj.material.albedoTex);
                                    obj.material.albedoTex = carregarTexturaDoDisco(pintor, selecionado.c_str());
                                    strncpy(bufAlbedo, selecionado.c_str(), sizeof(bufAlbedo)-1);
                                    bufAlbedo[255] = '\0';
                                }
                            }
                            
                            if (ImGui::InputText("Normal Map##mat", bufNormal, sizeof(bufNormal))) {
                                obj.material.normal = std::string(bufNormal);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("...##normal")) {
                                std::string selecionado = abrirSeletorArquivo("Selecionar Normal Map");
                                if (!selecionado.empty()) {
                                    obj.material.normal = selecionado;
                                    liberarTextura(obj.material.normalTex);
                                    obj.material.normalTex = carregarTexturaDoDisco(pintor, selecionado.c_str());
                                    strncpy(bufNormal, selecionado.c_str(), sizeof(bufNormal)-1);
                                    bufNormal[255] = '\0';
                                }
                            }
                            
                            if (ImGui::InputText("Roughness##mat", bufRough, sizeof(bufRough))) {
                                obj.material.roughness = std::string(bufRough);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("...##roughness")) {
                                std::string selecionado = abrirSeletorArquivo("Selecionar Roughness Map");
                                if (!selecionado.empty()) {
                                    obj.material.roughness = selecionado;
                                    liberarTextura(obj.material.roughnessTex);
                                    obj.material.roughnessTex = carregarTexturaDoDisco(pintor, selecionado.c_str());
                                    strncpy(bufRough, selecionado.c_str(), sizeof(bufRough)-1);
                                    bufRough[255] = '\0';
                                }
                            }

                            ImGui::TextWrapped("(Deixe em branco para usar cor sólida. Digite o caminho da imagem para usar textura.)");
                            if (ImGui::Button("Remover texturas personalizadas", ImVec2(-1, 0))) {
                                obj.material.albedo.clear();
                                obj.material.normal.clear();
                                obj.material.roughness.clear();
                                liberarTextura(obj.material.albedoTex);
                                liberarTextura(obj.material.normalTex);
                                liberarTextura(obj.material.roughnessTex);
                            }
                        }
                        ImGui::EndTabItem();
                    }

                    // --- Iluminação ---
                    if (ImGui::BeginTabItem("Iluminação")) {
                        ImGui::Text("Direcional");
                        if (!sceneLuzVisivel) {
                            ImGui::TextWrapped("Nenhuma luz adicionada. Use o botão 'Adicionar luz' no painel Criar Blocos.");
                        } else {
                            ImGui::TextWrapped("Posicione o bloco visualmente na viewport e ajuste sua intensidade.");
                            ImGui::Separator();
                            ImGui::DragFloat3("Posição", &sceneLuz.posicao.x, 0.1f);
                            ImGui::DragFloat("Intensidade", &sceneLuz.intensidade, 0.01f, 0.0f, 10.0f);
                            ImGui::TextWrapped("Arraste o bloco na viewport para mover a luz.");
                        }
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                ImGui::End();
            }

        // Engine Viewport Window
        if (showViewport) {
            ImGui::SetNextWindowPos(ImVec2(100, 50), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::Begin("Engine Viewport", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            ImGui::Text("Camera: (%.2f, %.2f, %.2f)", camera.pos.x, camera.pos.y, camera.pos.z);
            ImGui::Text("WASD: mover | Shift+Mouse: rotacionar | Click: selecionar");
            ImGui::Text("Space/Ctrl: sobe/desce | Cubos nas setas dos gizmos");

            ImGui::Separator();
            ImVec2 viewportSize = ImGui::GetContentRegionAvail();
            ImVec2 viewportPos = ImGui::GetCursorScreenPos();

            ImGui::BeginChild("ViewportChild", viewportSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
            ImGui::InvisibleButton("viewport_canvas", viewportSize);
            viewportFocado = ImGui::IsItemHovered();
            if (viewportFocado) {
                ImGuiIO& io = ImGui::GetIO();
                ultimoMouseViewport = ImVec2(io.MousePos.x - viewportPos.x, io.MousePos.y - viewportPos.y);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    if (sceneLuzVisivel && detectarLuzHandle(ultimoMouseViewport, camera, sceneLuz, (int)viewportSize.x, (int)viewportSize.y)) {
                        lightArrastando = true;
                    } else {
                        mouseClaqueouViewport = 1;
                    }
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && gizmo.cuboSelecionado >= 0 && gizmo.cuboSelecionado < (int)cubos.size()) {
                    gizmo.modo = proximoTipoGizmo(gizmo.modo);
                }
            }
            ImGui::EndChild();

            ImGui::End();
            ImGui::PopStyleColor(2);
            
                    // Renderiza o conteúdo do engine na área da viewport
            DesenharViewportEngine(pintor, camera, cubos, viewportPos, viewportSize, ultimoMouseViewport, mouseClaqueouViewport, gizmo, sceneLuz, sceneLuzVisivel, lightArrastando);
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), pintor);
        SDL_RenderPresent(pintor);

        SDL_Delay(16);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    // Limpa texturas de todos os cubos
    for (auto& cubo : cubos) {
        liberarTextura(cubo.material.albedoTex);
        liberarTextura(cubo.material.normalTex);
        liberarTextura(cubo.material.roughnessTex);
    }

    SDL_DestroyTexture(texturaFundo);
    SDL_DestroyTexture(iconViewport);
    SDL_DestroyTexture(iconBlocos);
    SDL_DestroyTexture(iconInspetor);
    SDL_DestroyRenderer(pintor);
    SDL_DestroyWindow(janela);
    SDL_Quit();
    return 0;
}
