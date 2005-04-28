
//#include <crtdbg.h>
#include "AY38900.h"
#include "core/cpu/ProcessorBus.h"

#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZRHW|D3DFVF_TEX1)
#define MIN(v1, v2) (v1 < v2 ? v1 : v2)
#define MAX(v1, v2) (v1 > v2 ? v1 : v2)

struct CUSTOMVERTEX
{
    FLOAT  x, y, z, rhw; // The position
    FLOAT  tu, tv;       // The texture coordinates
};

const INT32 AY38900::TICK_LENGTH_SCANLINE             = 228;
const INT32 AY38900::TICK_LENGTH_FRAME                = 59736;
const INT32 AY38900::TICK_LENGTH_VBLANK               = 15164;
const INT32 AY38900::TICK_LENGTH_START_ACTIVE_DISPLAY = 112;
const INT32 AY38900::TICK_LENGTH_IDLE_ACTIVE_DISPLAY  = 456;
const INT32 AY38900::TICK_LENGTH_FETCH_ROW            = 456;
const INT32 AY38900::TICK_LENGTH_RENDER_ROW           = 3192;
const INT32 AY38900::LOCATION_BACKTAB    = 0x0200;
const INT32 AY38900::LOCATION_GROM       = 0x3000;
const INT32 AY38900::LOCATION_GRAM       = 0x3800;
const INT32 AY38900::FOREGROUND_BIT		 = 0x0010;

const UINT32 AY38900::palette[32] = {
    0x000000, 0x002DFF, 0xFF3D10, 0xC9CFAB,
    0x386B3F, 0x00A756, 0xFAEA50, 0xFFFCFF,
    0xBDACC8, 0x24B8FF, 0xFFB41F, 0x546E00,
    0xFF4E57, 0xA496FF, 0x75CC80, 0xB51A58,
    0x000000, 0x002DFF, 0xFF3D10, 0xC9CFAB,
    0x386B3F, 0x00A756, 0xFAEA50, 0xFFFCFF,
    0xBDACC8, 0x24B8FF, 0xFFB41F, 0x546E00,
    0xFF4E57, 0xA496FF, 0x75CC80, 0xB51A58,
};

AY38900::AY38900(MemoryBus* mb, GROM* go, GRAM* ga)
	: Processor("AY-3-8900"),
      memoryBus(mb),
      grom(go),
      gram(ga),
	  backtab(),
	  vertexBuffer(NULL),
	  combinedTexture(NULL),
	  videoOutputDevice(NULL)
{
    registers.init(this);

    horizontalOffset = 0;
    verticalOffset   = 0;
    blockTop         = false;
    blockLeft        = false;
    mode             = 0;
}

int AY38900::getClockSpeed() {
    return 3579545;
}
        
void AY38900::resetVideoProducer() {
}

void AY38900::resetProcessor() {
    //switch to bus copy mode
    setGraphicsBusVisible(true);

    //reset the mobs
    for (UINT8 i = 0; i < 8; i++)
        mobs[i].reset();

    //reset the state variables
    mode = -1;
    pinOut[AY38900_PIN_OUT_SR1]->isHigh = TRUE;
    pinOut[AY38900_PIN_OUT_SR2]->isHigh = TRUE;
    previousDisplayEnabled = true;
    displayEnabled         = false;
    colorStackMode         = false;
    colorModeChanged       = true;
    bordersChanged         = true;
    colorStackChanged      = true;
    offsetsChanged         = true;

    //local register data
    borderColor = 0;
    blockLeft = blockTop = false;
    horizontalOffset = verticalOffset = 0;
}

void AY38900::setGraphicsBusVisible(BOOL visible) {
    registers.visible = visible;
    gram->visible = visible;
    grom->visible = visible;
}

int AY38900::tick(INT32 minimum) {
    int totalTicks = 0;
    do {
        //move to the next mode
        mode++;

        switch (mode) {
            case 0:
                //come out of bus isolation mode
                setGraphicsBusVisible(true);
                if (previousDisplayEnabled)
                    renderFrame();
                displayEnabled = false;
            
                //start of vblank, so stop and go back to the main loop
                processorBus->stop();

                //release SR2, allowing the CPU to run
                pinOut[AY38900_PIN_OUT_SR2]->isHigh = TRUE;

                //kick the irq line
                pinOut[AY38900_PIN_OUT_SR1]->isHigh = FALSE;

                totalTicks += TICK_LENGTH_VBLANK;
                break;
            case 1:
                pinOut[AY38900_PIN_OUT_SR1]->isHigh = TRUE;

                //if the display is not enabled, skip the rest of the modes
                if (!displayEnabled) {
                    previousDisplayEnabled = false;
                    if (previousDisplayEnabled) {
                        //render a blank screen
                        for (int x = 0; x < 160; x++)
                            for (int y = 0; x < 192; x++)
                                ((UINT32**)combinedBufferLock.pBits)[x][y] = palette[borderColor];
                    }
                    mode = -1;
                    totalTicks += (TICK_LENGTH_FRAME - TICK_LENGTH_VBLANK);
                }
                else {
                    previousDisplayEnabled = true;
                    pinOut[AY38900_PIN_OUT_SR2]->isHigh = FALSE;
                    totalTicks += TICK_LENGTH_START_ACTIVE_DISPLAY;
                }
                break;

            case 2:
                //switch to bus isolation mode, but only if the CPU has
                //acknowledged ~SR2 by asserting ~SST
                if (!pinIn[AY38900_PIN_IN_SST]->isHigh) {
                    pinIn[AY38900_PIN_IN_SST]->isHigh = TRUE;
                    setGraphicsBusVisible(false);
                }

                //release SR2
                pinOut[AY38900_PIN_OUT_SR2]->isHigh = TRUE;

                totalTicks += TICK_LENGTH_IDLE_ACTIVE_DISPLAY +
                    (2*verticalOffset*TICK_LENGTH_SCANLINE);
                break;

            case 3:
            case 5:
            case 7:
            case 9:
            case 11:
            case 13:
            case 15:
            case 17:
            case 19:
            case 21:
            case 23:
            case 25:
                pinOut[AY38900_PIN_OUT_SR2]->isHigh = FALSE;
                //renderRow((mode-3)/2);
                totalTicks += TICK_LENGTH_FETCH_ROW;
                break;

            case 4:
            case 6:
            case 8:
            case 10:
            case 12:
            case 14:
            case 16:
            case 18:
            case 20:
            case 22:
            case 24:
                pinOut[AY38900_PIN_OUT_SR2]->isHigh = TRUE;
                pinIn[AY38900_PIN_IN_SST]->isHigh = TRUE;
                totalTicks += TICK_LENGTH_RENDER_ROW;
                break;

            case 26:
                pinOut[AY38900_PIN_OUT_SR2]->isHigh = TRUE;

                //this mode could be cut off in tick length if the vertical
                //offset is greater than 1
            switch (verticalOffset) {
                case 0:
                    totalTicks += TICK_LENGTH_RENDER_ROW;
                    break;
                case 1:
                    mode = -1;
                    totalTicks += TICK_LENGTH_RENDER_ROW - TICK_LENGTH_SCANLINE;
                    break;
                default:
                    mode = -1;
                    totalTicks += (TICK_LENGTH_RENDER_ROW - TICK_LENGTH_SCANLINE -
                        (2*(verticalOffset-1)*TICK_LENGTH_SCANLINE));
                    break;
            }
                break;

            case 27:
            default:
                mode = -1;
                pinOut[AY38900_PIN_OUT_SR2]->isHigh = FALSE;
                totalTicks += TICK_LENGTH_SCANLINE;
                break;
        }

    } while (totalTicks < minimum);

    return totalTicks;
}

/*
void AY38900::renderRow(int rowNum) {
    UINT8 backTabPtr = (UINT8)(0x200+(rowNum*20));
    if (colorStackMode) {
        UINT8 csPtr = 0;
        UINT8 nextx = 0;
        UINT8 nexty = 0;
        for (UINT8 h = 0; h < 20; h++) {
            UINT8 nextCard = (UINT8)backtab.peek(backTabPtr);
            backTabPtr++;

            if ((nextCard & 0x1800) == 0x1000) {
                //colored squares mode
                UINT8 csColor = (UINT8)registers.memory[0x28 + csPtr];
                UINT8 color0 = (UINT8)(nextCard & 0x0007);
                UINT8 color1 = (UINT8)((nextCard & 0x0038) >> 3);
                UINT8 color2 = (UINT8)((nextCard & 0x01C0) >> 6);
                UINT8 color3 = (UINT8)(((nextCard & 0x2000) >> 11) |
                    ((nextCard & 0x0600) >> 9));
                renderColoredSquares(nextx, nexty,
                    (color0 == 7 ? csColor : (UINT8)(color0 | FOREGROUND_BIT)),
                    (color1 == 7 ? csColor : (UINT8)(color1 | FOREGROUND_BIT)),
                    (color2 == 7 ? csColor : (UINT8)(color2 | FOREGROUND_BIT)),
                    (color3 == 7 ? csColor : (UINT8)(color3 | FOREGROUND_BIT)));
            }
            else {
                //color stack mode
                //advance the color pointer, if necessary
                if ((nextCard & 0x2000) != 0)
                    csPtr = (UINT8)((csPtr+1) & 0x03);

                BOOL isGrom = (nextCard & 0x0800) == 0;
                UINT8 memoryLocation = (UINT8)(isGrom ? (nextCard & 0x07F8)
                    : (nextCard & 0x01F8));

                UINT8 fgcolor = (UINT8)(((nextCard & 0x1000) >> 9) |
                    (nextCard & 0x0007) | FOREGROUND_BIT);
                UINT8 bgcolor = (UINT8)registers.memory[0x28 + csPtr];
                UINT16* memory = (isGrom ? grom->image : gram->image);
                for (int j = 0; j < 8; j++)
                    renderLine((UINT8)memory[memoryLocation+j], nextx, nexty+j, fgcolor, bgcolor);
            }
            nextx += 8;
            if (nextx == 160) {
                nextx = 0;
                nexty += 8;
            }
        }
    }
    else {
        UINT8 nextx = 0;
        UINT8 nexty = 0;
        for (UINT8 i = 0; i < 240; i++) {
            UINT8 nextCard = (UINT8)backtab.peek((UINT8)(0x200+i));
            BOOL isGrom = (nextCard & 0x0800) == 0;
            BOOL renderAll = backtab.isDirty((UINT8)(0x200+i)) || colorModeChanged;
            UINT8 memoryLocation = (UINT8)(nextCard & 0x01F8);

            if (renderAll || (!isGrom && gram->isCardDirty(memoryLocation))) {
                UINT8 fgcolor = (UINT8)((nextCard & 0x0007) | FOREGROUND_BIT);
                UINT8 bgcolor = (UINT8)(((nextCard & 0x2000) >> 11) |
                    ((nextCard & 0x1600) >> 9));

                UINT16* memory = (isGrom ? grom->image : gram->image);
                for (int j = 0; j < 8; j++)
                    renderLine((UINT8)memory[memoryLocation+j], nextx, nexty+j, fgcolor, bgcolor);
            }
            nextx += 8;
            if (nextx == 160) {
                nextx = 0;
                nexty += 8;
            }
        }
    }
}
*/

void AY38900::renderFrame() {
    //render the next frame
    if (somethingChanged()) {
        renderBackground();
        renderMOBs();
        for (int i = 0; i < 8; i++)
            mobs[i].collisionRegister = 0;
        determineMOBCollisions();
        markClean();
    }
}

void AY38900::setVideoOutputDevice(IDirect3DDevice9* vod)
{
	if (this->videoOutputDevice != NULL) {
		if (vertexBuffer != NULL) {
			vertexBuffer->Release();
			vertexBuffer = NULL;
		}
		if (combinedTexture != NULL) {
			combinedTexture->Release();
			combinedTexture = NULL;
		}
	}

    this->videoOutputDevice = vod;

	if (this->videoOutputDevice != NULL) {
		//obtain the surface we desire
		videoOutputDevice->CreateTexture(160, 192, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8,
                D3DPOOL_DEFAULT, &combinedTexture, NULL);
	    
		//create our vertex buffer
		IDirect3DSurface9* bb;
		videoOutputDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
		D3DSURFACE_DESC desc;
		bb->GetDesc(&desc);
		videoOutputDevice->CreateVertexBuffer(6*sizeof(CUSTOMVERTEX), 0, D3DFVF_CUSTOMVERTEX, D3DPOOL_MANAGED, &vertexBuffer, NULL);
		CUSTOMVERTEX vertices[] =
		{
			{   0.0f,                0.0f,               1.0f, 1.0f, 0.0f, 0.0f, }, // x, y, z, rhw, tu, tv
			{   (FLOAT)desc.Width,   0.0f,               1.0f, 1.0f, 1.0f, 0.0f, },
			{   0.0f,                (FLOAT)desc.Height, 1.0f, 1.0f, 0.0f, 1.0f, },
			{   (FLOAT)desc.Width,   0.0f,               1.0f, 1.0f, 1.0f, 0.0f, },
			{   (FLOAT)desc.Width,   (FLOAT)desc.Height, 1.0f, 1.0f, 1.0f, 1.0f, },
			{   0.0f,                (FLOAT)desc.Height, 1.0f, 1.0f, 0.0f, 1.0f, },
		};
		bb->Release();
		void* pVertices;
		vertexBuffer->Lock(0, sizeof(vertices), (void**)&pVertices, 0);
		memcpy(pVertices, vertices, sizeof(vertices));
		vertexBuffer->Unlock();
	}
}

void AY38900::render()
{
	combinedTexture->LockRect(0, &combinedBufferLock, NULL, D3DLOCK_DISCARD |  D3DLOCK_NOSYSLOCK);
    renderBorders();
    copyBackgroundBufferToStagingArea();
    copyMOBsToStagingArea();
    for (int i = 0; i < 8; i++)
        registers.memory[0x18+i] |= mobs[i].collisionRegister;
	combinedTexture->UnlockRect(0);
	videoOutputDevice->SetTexture(0, combinedTexture);
	videoOutputDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	videoOutputDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    videoOutputDevice->SetTextureStageState( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	videoOutputDevice->SetFVF(D3DFVF_CUSTOMVERTEX);
    videoOutputDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	videoOutputDevice->SetStreamSource(0, vertexBuffer, 0, sizeof(CUSTOMVERTEX));
	videoOutputDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
}

BOOL AY38900::somethingChanged() {
    return (offsetsChanged || bordersChanged || colorStackChanged ||
        colorModeChanged || backtab.isDirty() || gram->isDirty() ||
        mobs[0].changed || mobs[1].changed ||
        mobs[2].changed || mobs[3].changed ||
        mobs[4].changed || mobs[5].changed ||
        mobs[6].changed || mobs[7].changed);
}

void AY38900::markClean() {
    //everything has been rendered and is now clean
    offsetsChanged = false;
    bordersChanged = false;
    colorStackChanged = false;
    colorModeChanged = false;
    backtab.markClean();
    gram->markClean();
    for (int i = 0; i < 8; i++)
        mobs[i].markClean();
}

void AY38900::renderBorders() {
    /*
    if (!bordersChanged && (!offsetsChanged || (blockLeft && blockTop)))
        return;
    */

    //draw the borders, if necessary
    if (blockTop) {
        for (UINT8 y = 0; y < 8; y++) {
            UINT32* buffer0 = ((UINT32*)combinedBufferLock.pBits) + (y*combinedBufferLock.Pitch/4);
            UINT32* buffer1 = buffer0 + (184*combinedBufferLock.Pitch/4);
            for (UINT8 x = 0; x < 160; x++) {
                *buffer0++ = palette[borderColor];
                *buffer1++ = palette[borderColor];
            }
        }
    }
    else if (verticalOffset != 0) {
        UINT8 numRows = (UINT8)(verticalOffset<<1);
        for (UINT8 y = 0; y < numRows; y++) {
            UINT32* buffer0 = ((UINT32*)combinedBufferLock.pBits) + (y*combinedBufferLock.Pitch/4);
            for (UINT8 x = 0; x < 160; x++)
                *buffer0++ = palette[borderColor];
        }
    }

    if (blockLeft) {
        for (UINT8 y = 0; y < 192; y++) {
            UINT32* buffer0 = ((UINT32*)combinedBufferLock.pBits) + (y*combinedBufferLock.Pitch/4);
            UINT32* buffer1 = buffer0 + 156;
            for (UINT8 x = 0; x < 4; x++) {
                *buffer0++ = palette[borderColor];
                *buffer1++ = palette[borderColor];
            }
        }
    }
    else if (horizontalOffset != 0) {
        for (UINT8 y = 0; y < 192; y++) { 
            UINT32* buffer0 = ((UINT32*)combinedBufferLock.pBits) + (y*combinedBufferLock.Pitch/4);
            for (UINT8 x = 0; x < horizontalOffset; x++) {
                *buffer0++ = palette[borderColor];
            }
        }
    }
}

void AY38900::renderMOBs()
{
    MOBRect* r;
    int cardNumber;
    int cardMemoryLocation;
    int pixelSize;
    int mobPixelHeight;
    BOOL doubleX;
    UINT16 nextMemoryLocation;
    int nextData;
    int nextX;
    int nextY;
    int xInc;

    for (int i = 0; i < 8; i++) {
        if (!mobs[i].changed && mobs[i].isGrom)
            continue;

        cardNumber = mobs[i].cardNumber;
        if (!mobs[i].isGrom)
            cardNumber = (cardNumber & 0x003F);
        cardMemoryLocation = (cardNumber << 3);

        r = mobs[i].getBounds();
        pixelSize = (mobs[i].quadHeight ? 4 : 1) *
            (mobs[i].doubleHeight ? 2 : 1);
        mobPixelHeight = 2 * r->height;
        doubleX = mobs[i].doubleWidth;

        for (int j = 0; j < mobPixelHeight; j++) {
            nextMemoryLocation = (UINT16)(cardMemoryLocation + (j/pixelSize));
            //if (!mobs[i].changed && !gram->isDirty(nextMemoryLocation))
            //continue;

            /*
                        nextData = memoryBus.peek(nextMemoryLocation);
            */
            nextData = (mobs[i].isGrom
                ? (nextMemoryLocation >= (int)grom->getSize()
                ? (UINT16)0xFFFF : grom->peek(nextMemoryLocation))
                : (nextMemoryLocation >= (int)gram->getSize()
                ? (UINT16)0xFFFF: gram->peek(nextMemoryLocation)));
            nextX = (mobs[i].horizontalMirror ? (doubleX ? 15 : 7) : 0);
            nextY = (mobs[i].verticalMirror
                ? (mobPixelHeight - j - 1) : j);
            xInc = (mobs[i].horizontalMirror ? -1: 1);
            mobBuffers[i][nextX][nextY] = ((nextData & 0x0080) != 0);
            mobBuffers[i][nextX + xInc][nextY] = (doubleX
                ? ((nextData & 0x0080) != 0)
                : ((nextData & 0x0040) != 0));
            mobBuffers[i][nextX + (2*xInc)][nextY] = (doubleX
                ? ((nextData & 0x0040) != 0)
                : ((nextData & 0x0020) != 0));
            mobBuffers[i][nextX + (3*xInc)][nextY] = (doubleX
                ? ((nextData & 0x0040) != 0)
                : ((nextData & 0x0010) != 0));
            mobBuffers[i][nextX + (4*xInc)][nextY] = (doubleX
                ? ((nextData & 0x0020) != 0)
                : ((nextData & 0x0008) != 0));
            mobBuffers[i][nextX + (5*xInc)][nextY] = (doubleX
                ? ((nextData & 0x0020) != 0)
                : ((nextData & 0x0004) != 0));
            mobBuffers[i][nextX + (6*xInc)][nextY] = (doubleX
                ? ((nextData & 0x0010) != 0)
                : ((nextData & 0x0002) != 0));
            mobBuffers[i][nextX + (7*xInc)][nextY] = (doubleX
                ? ((nextData & 0x0010) != 0)
                : ((nextData & 0x0001) != 0));
            if (!doubleX)
                continue;

            mobBuffers[i][nextX + (8*xInc)][nextY] =
                ((nextData & 0x0008) != 0);
            mobBuffers[i][nextX + (9*xInc)][nextY] =
                ((nextData & 0x0008) != 0);
            mobBuffers[i][nextX + (10*xInc)][nextY] =
                ((nextData & 0x0004) != 0);
            mobBuffers[i][nextX + (11*xInc)][nextY] =
                ((nextData & 0x0004) != 0);
            mobBuffers[i][nextX + (12*xInc)][nextY] =
                ((nextData & 0x0002) != 0);
            mobBuffers[i][nextX + (13*xInc)][nextY] =
                ((nextData & 0x0002) != 0);
            mobBuffers[i][nextX + (14*xInc)][nextY] =
                ((nextData & 0x0001) != 0);
            mobBuffers[i][nextX + (15*xInc)][nextY] =
                ((nextData & 0x0001) != 0);
        }

    }
}

void AY38900::renderBackground() {
    if (backtab.isDirty() || gram->isDirty() || colorStackChanged ||
        colorModeChanged) {
        if (colorStackMode)
            renderColorStackMode();
        else
            renderForegroundBackgroundMode();
    }
}

void AY38900::renderForegroundBackgroundMode()
{
    UINT8 nextx = 0;
    UINT8 nexty = 0;
    for (UINT8 i = 0; i < 240; i++) {
        UINT16 nextCard = backtab.peek((0x200+i));
        BOOL isGrom = (nextCard & 0x0800) == 0;
        BOOL renderAll = backtab.isDirty(0x200+i) || colorModeChanged;
        UINT16 memoryLocation = nextCard & 0x01F8;

        if (renderAll || (!isGrom && gram->isCardDirty(memoryLocation))) {
            UINT8 fgcolor = (UINT8)((nextCard & 0x0007) | FOREGROUND_BIT);
            UINT8 bgcolor = (UINT8)(((nextCard & 0x2000) >> 11) |
                ((nextCard & 0x1600) >> 9));

            Memory* memory = (isGrom ? (Memory*)grom : (Memory*)gram);
            UINT16 address = memory->getAddress()+memoryLocation;
            for (UINT16 j = 0; j < 8; j++)
                renderLine((UINT8)memory->peek(address+j), nextx, nexty+j, fgcolor, bgcolor);
        }
        nextx += 8;
        if (nextx == 160) {
            nextx = 0;
            nexty += 8;
        }
    }
}

void AY38900::renderColorStackMode() {
    UINT8 csPtr = 0;
    //if there are any dirty color advance bits in the backtab, or if
    //the color stack or the color mode has changed, the whole scene
    //must be rendered
    BOOL renderAll = backtab.areColorAdvanceBitsDirty() ||
        colorStackChanged || colorModeChanged;

    UINT8 nextx = 0;
    UINT8 nexty = 0;
    for (UINT8 h = 0; h < 240; h++) {
        UINT16 nextCard = backtab.peek(0x200+h);

        //_ASSERT(_CrtCheckMemory());
        //colored squares mode
        if ((nextCard & 0x1800) == 0x1000) {
            if (renderAll || backtab.isDirty(0x200+h)) {
                UINT8 csColor = (UINT8)registers.memory[0x28 + csPtr];
                UINT8 color0 = (UINT8)(nextCard & 0x0007);
                UINT8 color1 = (UINT8)((nextCard & 0x0038) >> 3);
                UINT8 color2 = (UINT8)((nextCard & 0x01C0) >> 6);
                UINT8 color3 = (UINT8)(((nextCard & 0x2000) >> 11) |
                    ((nextCard & 0x0600) >> 9));
                renderColoredSquares(nextx, nexty,
                    (color0 == 7 ? csColor : (UINT8)(color0 | FOREGROUND_BIT)),
                    (color1 == 7 ? csColor : (UINT8)(color1 | FOREGROUND_BIT)),
                    (color2 == 7 ? csColor : (UINT8)(color2 | FOREGROUND_BIT)),
                    (color3 == 7 ? csColor : (UINT8)(color3 | FOREGROUND_BIT)));
            }
        }
        //color stack mode
        else {
            //advance the color pointer, if necessary
            if ((nextCard & 0x2000) != 0)
                csPtr = (UINT8)((csPtr+1) & 0x03);

            BOOL isGrom = (nextCard & 0x0800) == 0;
            UINT16 memoryLocation = (isGrom ? (nextCard & 0x07F8)
                : (nextCard & 0x01F8));

            if (renderAll || backtab.isDirty(0x200+h) ||
                (!isGrom && gram->isCardDirty(memoryLocation))) {
                UINT8 fgcolor = (UINT8)(((nextCard & 0x1000) >> 9) |
                    (nextCard & 0x0007) | FOREGROUND_BIT);
                UINT8 bgcolor = (UINT8)registers.memory[0x28 + csPtr];
                Memory* memory = (isGrom ? (Memory*)grom : (Memory*)gram);
                UINT16 address = memory->getAddress()+memoryLocation;
                for (UINT16 j = 0; j < 8; j++)
                    renderLine((UINT8)memory->peek(address+j), nextx, nexty+j, fgcolor, bgcolor);
            }
        }
        nextx += 8;
        if (nextx == 160) {
            nextx = 0;
            nexty += 8;
        }
    }
}

void AY38900::copyBackgroundBufferToStagingArea()
{
    int sourceWidthX = blockLeft ? 152 : (160 - horizontalOffset);
    int sourceHeightY = blockTop ? 88 : (96 - verticalOffset);

    int nextSourcePixel = (blockLeft ? (8 - horizontalOffset) : 0) +
        ((blockTop ? (8 - verticalOffset) : 0) * 160);
    for (int y = 0; y < sourceHeightY; y++) {
		UINT32* nextPixelStore0 = (UINT32*)combinedBufferLock.pBits;
		nextPixelStore0 += (y*combinedBufferLock.Pitch)>>1;
		if (blockTop) nextPixelStore0 += combinedBufferLock.Pitch<<1;
		if (blockLeft) nextPixelStore0 += 4;
		UINT32* nextPixelStore1 = nextPixelStore0 + combinedBufferLock.Pitch/4;
        for (int x = 0; x < sourceWidthX; x++) {
			UINT32 nextColor = palette[backgroundBuffer[nextSourcePixel+x]];
			*nextPixelStore0++ = nextColor;
			*nextPixelStore1++ = nextColor;
        }
        nextSourcePixel += 160;
    }
}

//copy the offscreen mob buffers to the staging area
void AY38900::copyMOBsToStagingArea()
{
    for (INT8 i = 7; i >= 0; i--) {
        if (mobs[i].xLocation == 0 ||
            (!mobs[i].flagCollisions && !mobs[i].isVisible))
            continue;

        BOOL borderCollision = false;
        BOOL foregroundCollision = false;

        MOBRect* r = mobs[i].getBounds();
        UINT8 mobPixelHeight = (UINT8)(r->height << 1);
        UINT8 fgcolor = (UINT8)mobs[i].foregroundColor;

        short leftX = (short)(r->x + horizontalOffset);
        short nextY = (short)((r->y + verticalOffset) << 1);
        for (UINT8 y = 0; y < mobPixelHeight; y++) {
            for (UINT8 x = 0; x < r->width; x++) {
                //if this mob pixel is not on, then our life has no meaning
                if (!mobBuffers[i][x][y])
                    continue;

                //if the next pixel location is on the border, then we
                //have a border collision and we can ignore painting it
                int nextX = leftX + x;
                if (nextX < (blockLeft ? 8 : 0) || nextX > 158 ||
                    nextY < (blockTop ? 16 : 0) || nextY > 191) {
                    borderCollision = true;
                    continue;
                }

                //check for foreground collision
                UINT8 currentPixel = backgroundBuffer[(r->x+x)+ ((r->y+(y/2))*160)];
                if ((currentPixel & FOREGROUND_BIT) != 0) {
                    foregroundCollision = true;
                    if (mobs[i].behindForeground)
                        continue;
                }

                if (mobs[i].isVisible) {
					UINT32* nextPixel = (UINT32*)combinedBufferLock.pBits;
					nextPixel += leftX - (blockLeft ? 4 : 0) + x;
					nextPixel += (nextY - (blockTop ? 8 : 0)) * (combinedBufferLock.Pitch/4);
					*nextPixel = palette[fgcolor | (currentPixel & FOREGROUND_BIT)];
					/*
					((UINT32*)combinedBufferLock.pBits)[leftX - (blockLeft ? 4 : 0) + x + ((nextY - (blockTop ? 8 : 0))*(combinedBufferLock.Pitch/4))] = 
                        palette[fgcolor | (currentPixel & FOREGROUND_BIT)];
				    */
                }
            }
            //rowPixelIndex += 160;
            nextY++;
        }

        //update the collision bits
        if (mobs[i].flagCollisions) {
            if (foregroundCollision)
                mobs[i].collisionRegister |= 0x0100;
            if (borderCollision)
                mobs[i].collisionRegister |= 0x0200;
        }
    }
}

void AY38900::renderLine(UINT8 nextbyte, int x, int y, UINT8 fgcolor, UINT8 bgcolor)
{
    int nextTargetPixel = x + (y*160);
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x80) != 0 ? fgcolor : bgcolor;
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x40) != 0 ? fgcolor : bgcolor;
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x20) != 0 ? fgcolor : bgcolor;
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x10) != 0 ? fgcolor : bgcolor;
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x08) != 0 ? fgcolor : bgcolor;
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x04) != 0 ? fgcolor : bgcolor;
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x02) != 0 ? fgcolor : bgcolor;
    backgroundBuffer[nextTargetPixel++] = (nextbyte & 0x01) != 0 ? fgcolor : bgcolor;
}

void AY38900::renderColoredSquares(int x, int y, UINT8 color0, UINT8 color1,
    UINT8 color2, UINT8 color3) {
    int topLeftPixel = x + (y*160);
    int topRightPixel = topLeftPixel+4;
    int bottomLeftPixel = topLeftPixel+640;
    int bottomRightPixel = bottomLeftPixel+4;

    for (UINT8 w = 0; w < 4; w++) {
        for (UINT8 i = 0; i < 4; i++) {
            backgroundBuffer[topLeftPixel++] = color0;
            backgroundBuffer[topRightPixel++] = color1;
            backgroundBuffer[bottomLeftPixel++] = color2;
            backgroundBuffer[bottomRightPixel++] = color3;
        }
        topLeftPixel += 156;
        topRightPixel += 156;
        bottomLeftPixel += 156;
        bottomRightPixel += 156;
    }
}

void AY38900::determineMOBCollisions() {
    //check mob to mob collisions
    for (int i = 0; i < 7; i++) {
        if (mobs[i].xLocation == 0 || !mobs[i].flagCollisions)
            continue;

        for (int j = i+1; j < 8; j++) {
            if (mobs[j].xLocation == 0 || !mobs[j].flagCollisions)
                continue;

            if (mobsCollide(i, j)) {
                mobs[i].collisionRegister |= (UINT8)(1 << j);
                mobs[j].collisionRegister |= (UINT8)(1 << i);
            }
        }
    }
}

BOOL AY38900::mobsCollide(int mobNum0, int mobNum1) {
    MOBRect* r0;
    r0 = mobs[mobNum0].getBounds();
    MOBRect* r1;
    r1 = mobs[mobNum1].getBounds();
    if (!r0->intersects(r1))
        return false;

    //iterate over the intersecting bits to see if any touch
    int x0 = MAX(r0->x, r1->x);
    int y0 = MAX(r0->y, r1->y);
    int r0y = 2*(y0-r0->y);
    int r1y = 2*(y0-r1->y);
    int width = MIN(r0->x+r0->width, r1->x+r1->width) - x0;
    int height = (MIN(r0->y+r0->height, r1->y+r1->height) - y0) * 2;
    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            if (mobBuffers[mobNum0][x0-r0->x+x][r0y+y] &&
                mobBuffers[mobNum1][x0-r1->x+x][r1y+y])
                return true;
        }
    }

    return false;
}
